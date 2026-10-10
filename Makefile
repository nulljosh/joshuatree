CC := clang
CFLAGS := -target i386-unknown-none -ffreestanding -fno-stack-protector \
          -fno-pic -mno-sse -mno-mmx -fno-omit-frame-pointer -Wall -Wextra -O2 \
          -Iboot -Ikernel -Idrivers -Ilib -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -Ithird_party/bearssl/shim -MMD -MP
LD := ld.lld
# -fno-omit-frame-pointer: kernel/backtrace.c walks the EBP chain to print
# crash-report frames (kernel/symtab.h). Without it clang's -O2 elides EBP
# as a general-purpose register and the chain walk has nothing to follow.

KERNEL_SRCS := kernel/gdt.c kernel/idt.c kernel/pic.c kernel/irq.c kernel/pmm.c \
               kernel/paging.c kernel/kheap.c kernel/task.c kernel/exec.c kernel/ring3.c kernel/ring3app.c kernel/syscall.c kernel/brk.c kernel/shellsys.c kernel/r3stress.c \
               kernel/gui_prims.c kernel/gui_paint.c kernel/dock_geom.c kernel/dock_draw.c kernel/app.c kernel/backtrace.c kernel/entropy.c kernel/auth_kdf.c kernel/kernel.c
KERNEL_ASM  := kernel/isr.S kernel/irq_stubs.S kernel/ring3_asm.S
DRIVER_SRCS := drivers/ata.c drivers/blockdev.c drivers/ramdisk.c drivers/trash.c drivers/fat.c drivers/vfs.c drivers/ramfs.c drivers/pci.c drivers/vbe.c drivers/mouse.c drivers/vmmouse.c \
               drivers/window.c drivers/rtl8139.c drivers/ne2k.c drivers/net.c drivers/http.c drivers/html.c \
               drivers/json.c drivers/font.c \
               drivers/serial.c drivers/sb16.c drivers/speak.c drivers/inflate.c drivers/png.c drivers/jpeg.c drivers/ttf.c
LIB_SRCS    := lib/libc.c third_party/bearssl/src/sha2small.c third_party/bearssl/src/hmac.c \
               third_party/bearssl/src/hmac_drbg.c third_party/bearssl/src/dec32be.c third_party/bearssl/src/enc32be.c

OBJS := boot/boot.o $(KERNEL_ASM:.S=.o) $(KERNEL_SRCS:.c=.o) $(DRIVER_SRCS:.c=.o) $(LIB_SRCS:.c=.o) kernel/symtab.o
PASS1_OBJS := $(filter-out kernel/symtab.o,$(OBJS))

# Crash-report symbol table: a two-pass build. Pass 1 links every object
# except kernel/symtab.o into kernel.elf.pass1 -- its .text addresses are
# final the moment PASS1_OBJS is complete, since boot/linker.ld places
# .text before .rodata/.data/.bss and symtab.o contributes only a data
# table, never code, so it can't move anything pass 1 already placed.
# gen_symtab.py then reads that binary's own `nm -n` into kernel/symtab.c,
# which compiles into the real kernel/symtab.o for the final link below.
# Neither kernel.elf.pass1 nor the generated kernel/symtab.c is committed
# (see .gitignore); both regenerate from scratch on every build, same as
# drivers/version.h.
# boot/memmap.ld: kernel/memmap.h's #defines as linker-script symbols, so
# boot/linker.ld and user/*.ld INCLUDE the same ring-3 addresses the kernel
# compiles against. Generated, not committed (see .gitignore).
boot/memmap.ld: kernel/memmap.h
	sed -n 's/^#define \(JT_[A-Z_]*\) *\(0x[0-9A-Fa-f]*\).*/\1 = \2;/p' $< > $@

kernel.elf.pass1: $(PASS1_OBJS) kernel/symtab_stub.o boot/linker.ld boot/memmap.ld
	$(LD) -m elf_i386 -T boot/linker.ld -o $@ $(PASS1_OBJS) kernel/symtab_stub.o

kernel/symtab.c: kernel.elf.pass1 tools/gen/gen_symtab.py
	python3 tools/gen/gen_symtab.py kernel.elf.pass1 kernel/symtab.c

kernel/symtab.o: kernel/symtab.c kernel/symtab.h

kernel.elf: $(OBJS) boot/linker.ld boot/memmap.ld
	$(LD) -m elf_i386 -T boot/linker.ld -o $@ $(OBJS)
	@cp kernel.elf landing/v86/kernel.elf
	@gzip -9nc kernel.elf > landing/v86/kernel.elf.gz
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
               -fno-pic -mno-sse -mno-mmx -Wall -Wextra -Os -Ithird_party/minimp3 -Iuser

user/hello.o: user/hello.c user/jtsys.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/hello.bin: user/hello.o user/hello.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/hello.ld --oformat binary -o $@ user/hello.o

user/note.o: user/note.c user/jtsys.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/note.bin: user/note.o user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/note.o

# libjt: a small static archive of userland library code (string/ctype/
# stdlib/stdio over jtsys.h), built with the same freestanding flags as
# every other ring-3 program so a program can link it in and get libc
# shaped calls without a real libc or a kernel include path. llvm-ar
# rather than plain `ar` because this toolchain is clang/lld throughout,
# see USER_CFLAGS above.
LIBJT_SRCS := user/libjt/string.c user/libjt/stdlib.c user/libjt/stdio.c user/libjt/text.c user/libjt/osk.c user/libjt/mono.c user/libjt/jpeg.c user/libjt/mp3.c
LIBJT_OBJS := $(LIBJT_SRCS:.c=.o)
# Plain `llvm-ar` first (on PATH on most CI images); then a versioned
# `llvm-ar-NN` apt sometimes installs instead of the unversioned name;
# then the Homebrew Cellar path a bare macOS dev shell needs since
# neither of the above is on its PATH by default -- macOS's own
# /usr/bin/ar is BSD ar, which cannot index a foreign-format (ELF i386)
# archive (it silently produces one lld then fails to find symbols in,
# "not a mach-o file"), so it must never be reached on a Mac. Last,
# plain `ar` (GNU binutils, installed by default on Ubuntu runners and
# perfectly able to index ELF i386 objects there) for CI when no
# llvm-ar is present at all.
AR := $(shell command -v llvm-ar 2>/dev/null; \
             ls /usr/bin/llvm-ar-* /usr/lib/llvm-*/bin/llvm-ar 2>/dev/null | sort -V | tail -1; \
             ls /opt/homebrew/opt/llvm/bin/llvm-ar 2>/dev/null; \
             command -v ar 2>/dev/null)
AR := $(firstword $(AR))

user/libjt/%.o: user/libjt/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

# 1.9.23: antialiased ring-3 text. aafont.h is baked from the kernel's own
# DejaVu data through drivers/ttf.c by a host program; text.o needs it.
user/libjt/aafont.h: tools/gen/gen_user_text.c drivers/ttf.c drivers/ttf.h drivers/dejavu_font.h drivers/dejavu_bold_font.h drivers/dejavu_mono_font.h
	clang -O2 -DTTF_HOST_BUILD -Itools/ttf-host -Idrivers -o /tmp/jt-gen-user-text tools/gen/gen_user_text.c drivers/ttf.c -lm
	/tmp/jt-gen-user-text $@ user/libjt/aamono.h

# aamono.h (the mono face's own atlas) is written by the same generator run.
user/libjt/aamono.h: user/libjt/aafont.h
	@test -f $@

user/libjt/text.o user/libjt/mono.o: lib/text_ink.h

user/libjt/text.o: user/libjt/text.c user/libjt/text.h user/libjt/aafont.h user/jtsys.h
user/libjt/mono.o: user/libjt/mono.c user/libjt/text.h user/libjt/aamono.h user/jtsys.h
user/libjt/jpeg.o: user/libjt/jpeg.c drivers/jpeg.c drivers/jpeg.h user/libjt/string.h user/libjt/stdlib.h
user/libjt/mp3.o: user/libjt/mp3.c user/libjt/mp3.h third_party/minimp3/minimp3.h third_party/minimp3/stdlib.h third_party/minimp3/string.h
user/libjt/osk.o: user/libjt/osk.c user/libjt/osk.h user/libjt/text.h user/jtsys.h

user/libjt.a: $(LIBJT_OBJS)
	$(AR) rcs $@ $(LIBJT_OBJS)

user/wc.o: user/wc.c user/jtsys.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/wc.bin: user/wc.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/wc.o user/libjt.a

# 1.7.7: Keyrate as a ring-3 program (kernel/ring3app.c launches it from
# the dock). Same flags, same link script, same flat image as the others;
# it also pulls in drivers/vgafont.h as plain data for its glyphs.
user/keyrate.o: user/keyrate.c user/jtsys.h drivers/vgafont.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/keyrate.bin: user/keyrate.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/keyrate.o user/libjt.a

# 1.7.11: Toroid, the second app out of the kernel, built the same way.
user/toroid.o: user/toroid.c user/jtsys.h drivers/vgafont.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/toroid.bin: user/toroid.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/toroid.o user/libjt.a

# 1.7.12: Calculator, the third app out of the kernel, built the same way.
user/calculator.o: user/calculator.c user/jtsys.h drivers/vgafont.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/calculator.bin: user/calculator.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/calculator.o user/libjt.a

# 1.7.14: Quotes, the fourth app out of the kernel, built the same way.
user/quotes.o: user/quotes.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/quotes.bin: user/quotes.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/quotes.o user/libjt.a

# 2.0: Bookrank, the fifth app out of the kernel, built the same way.
user/bookrank.o: user/bookrank.c user/jtsys.h drivers/vgafont.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/bookrank.bin: user/bookrank.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/bookrank.o user/libjt.a

# 1.9.1: Tonchi, the seventh app out of the kernel, built the same way.
user/tonchi.o: user/tonchi.c user/jtsys.h drivers/vgafont.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/tonchi.bin: user/tonchi.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/tonchi.o user/libjt.a

# 1.9.3: Fieldbook, the ninth app out of the kernel, built the same way.
user/fieldbook.o: user/fieldbook.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/fieldbook.bin: user/fieldbook.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/fieldbook.o user/libjt.a

# 1.9.4: Clock, the tenth app out of the kernel, built the same way.
user/clock.o: user/clock.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/clock.bin: user/clock.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/clock.o user/libjt.a

# 2.2: Music, a ring-3 app that plays WAV (and MP3 through the hook in user/music.c). wav.o is
# linked in directly rather than through libjt.a so the shared LIBJT_SRCS line stays untouched.
user/music.o: user/music.c user/jtsys.h user/libjt/text.h user/libjt/wav.h user/libjt/mp3.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/music.bin: user/music.o user/libjt/wav.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/music.o user/libjt/wav.o user/libjt.a

drivers/user_music.h: user/music.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/music.bin drivers/user_music.h user_music

kernel/ring3app.o: drivers/user_music.h

# 1.9.5: Portfolio, the eleventh app out of the kernel, built the same way.
user/portfolio.o: user/portfolio.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/portfolio.bin: user/portfolio.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/portfolio.o user/libjt.a

# 1.9.6: Activity, the twelfth app out of the kernel, built the same way.
# 1.9.8: Hikko, the fourteenth app out of the kernel, built the same way.
user/hikko.o: user/hikko.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/hikko.bin: user/hikko.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/hikko.o user/libjt.a

# 1.9.7: Contacts, the thirteenth app out of the kernel, built the same way.
# 1.9.12: Search, the sixteenth app out of the kernel, built the same way.
user/search.o: user/search.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/search.bin: user/search.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/search.o user/libjt.a

# 1.9.9: Reminders, the fifteenth app out of the kernel, built the same way.
user/reminders.o: user/reminders.c user/jtsys.h drivers/vgafont.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/reminders.bin: user/reminders.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/reminders.o user/libjt.a

# 1.9.11: Curbfind, the sixteenth app out of the kernel, built the same way.
user/curbfind.o: user/curbfind.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/curbfind.bin: user/curbfind.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/curbfind.o user/libjt.a

# 1.9.12: Calendar, the seventeenth app out of the kernel, built the same way.
user/calendar.o: user/calendar.c user/jtsys.h drivers/vgafont.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/calendar.bin: user/calendar.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/calendar.o user/libjt.a

# 1.9.19: Epiphany, the nineteenth app out of the kernel, on SYS_HTTP_GET like Curbfind.
user/epiphany.o: user/epiphany.c user/jtsys.h drivers/vgafont.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/epiphany.bin: user/epiphany.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/epiphany.o user/libjt.a

# Burrow, the Files app, as a ring-3 program (not yet in RING3_APPS).
user/burrow.o: user/burrow.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/burrow.bin: user/burrow.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/burrow.o user/libjt.a

# Mail as a ring-3 program.
user/mail.o: user/mail.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/mail.bin: user/mail.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/mail.o user/libjt.a

# Samantha as a ring-3 program (dock slot 6, the shell commands and phone mode all open it).
user/samantha.o: user/samantha.c user/samcaps.h user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/samantha.bin: user/samantha.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/samantha.o user/libjt.a

# Notes browse view as a ring-3 program (slice 1; not yet in RING3_APPS).
user/notes.o: user/notes.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/notes.bin: user/notes.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/notes.o user/libjt.a

# The desktop Terminal, the twenty-fifth ring-3 app: SYS_SHELL_RUN for commands.
user/terminal.o: user/terminal.c user/jtsys.h user/libjt/text.h user/shellcore.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/terminal.bin: user/terminal.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/terminal.o user/libjt.a

# 1.9.22: Weather, the twentieth, reads the kernel's WEATHER.TXT.
user/weather.o: user/weather.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/weather.bin: user/weather.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/weather.o user/libjt.a

# Stocks as a ring-3 program (not yet in RING3_APPS): reads the kernel's STOCKS.TXT.
user/stocks.o: user/stocks.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/stocks.bin: user/stocks.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/stocks.o user/libjt.a

user/contacts.o: user/contacts.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/contacts.bin: user/contacts.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/contacts.o user/libjt.a

user/activity.o: user/activity.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/activity.bin: user/activity.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/activity.o user/libjt.a

# 1.7.8: fbpoke, the program that pokes the released window framebuffer
# and must fault. Run by kernel/ring3app.c under the `fbpoke` boot flag.
# 1.9.27: brkpoke, the SYS_BRK leak probe. Run by kernel/ring3app.c under the `brkpoke` boot flag.
user/brkpoke.o: user/brkpoke.c user/jtsys.h kernel/memmap.h
	$(CC) $(USER_CFLAGS) -c -o $@ user/brkpoke.c
user/brkpoke.bin: user/brkpoke.o user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/brkpoke.o
drivers/user_brkpoke.h: user/brkpoke.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/brkpoke.bin drivers/user_brkpoke.h user_brkpoke

user/fbpoke.o: user/fbpoke.c user/jtsys.h kernel/memmap.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/fbpoke.bin: user/fbpoke.o user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/fbpoke.o

# The built binaries, embedded so `usertest`/`notetest`/`shell` can seed
# them into the VFS on a machine with no disk (every headless check boot,
# and the browser embed).
drivers/user_hello.h: user/hello.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/hello.bin drivers/user_hello.h user_hello

drivers/user_note.h: user/note.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/note.bin drivers/user_note.h user_note

drivers/user_wc.h: user/wc.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/wc.bin drivers/user_wc.h user_wc

drivers/user_keyrate.h: user/keyrate.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/keyrate.bin drivers/user_keyrate.h user_keyrate

drivers/user_toroid.h: user/toroid.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/toroid.bin drivers/user_toroid.h user_toroid

drivers/user_calculator.h: user/calculator.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/calculator.bin drivers/user_calculator.h user_calculator

drivers/user_quotes.h: user/quotes.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/quotes.bin drivers/user_quotes.h user_quotes

drivers/user_bookrank.h: user/bookrank.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/bookrank.bin drivers/user_bookrank.h user_bookrank

drivers/user_tonchi.h: user/tonchi.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/tonchi.bin drivers/user_tonchi.h user_tonchi

drivers/user_fieldbook.h: user/fieldbook.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/fieldbook.bin drivers/user_fieldbook.h user_fieldbook

drivers/user_clock.h: user/clock.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/clock.bin drivers/user_clock.h user_clock

drivers/user_portfolio.h: user/portfolio.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/portfolio.bin drivers/user_portfolio.h user_portfolio

drivers/user_contacts.h: user/contacts.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/contacts.bin drivers/user_contacts.h user_contacts

drivers/user_hikko.h: user/hikko.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/hikko.bin drivers/user_hikko.h user_hikko

drivers/user_reminders.h: user/reminders.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/reminders.bin drivers/user_reminders.h user_reminders

drivers/user_curbfind.h: user/curbfind.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/curbfind.bin drivers/user_curbfind.h user_curbfind
drivers/user_calendar.h: user/calendar.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/calendar.bin drivers/user_calendar.h user_calendar
drivers/user_search.h: user/search.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/search.bin drivers/user_search.h user_search

drivers/user_epiphany.h: user/epiphany.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/epiphany.bin drivers/user_epiphany.h user_epiphany

drivers/user_burrow.h: user/burrow.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/burrow.bin drivers/user_burrow.h user_burrow

drivers/user_notes.h: user/notes.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/notes.bin drivers/user_notes.h user_notes
drivers/user_terminal.h: user/terminal.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/terminal.bin drivers/user_terminal.h user_terminal

drivers/user_samantha.h: user/samantha.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/samantha.bin drivers/user_samantha.h user_samantha

# Movies (2.2): motion-JPEG AVI player. Links the AVI reader straight in (not in libjt.a, so Music's LIBJT_SRCS edit never collides).
user/avi.o: user/libjt/avi.c user/libjt/avi.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/movies.o: user/movies.c user/jtsys.h user/libjt/text.h user/libjt/avi.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/movies.bin: user/movies.o user/avi.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/movies.o user/avi.o user/libjt.a

drivers/user_movies.h: user/movies.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/movies.bin drivers/user_movies.h user_movies

kernel/ring3app.o: drivers/user_movies.h

# Hamurabi (2.7): the 1968 city game, playable. The sprite sheet is a committed header (user/hamurabi_sprites.h from
# art/hamurabi/sprites.png); the rules and the story words are plain C headers it includes (hamurabi_rules.h, hamurabi_story.h).
user/hamurabi.o: user/hamurabi.c user/jtsys.h user/libjt/text.h user/hamurabi_sprites.h user/hamurabi_rules.h user/hamurabi_story.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/hamurabi.bin: user/hamurabi.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/hamurabi.o user/libjt.a

drivers/user_hamurabi.h: user/hamurabi.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/hamurabi.bin drivers/user_hamurabi.h user_hamurabi

kernel/ring3app.o: drivers/user_hamurabi.h

# Windgate (2.8): guided breathing, a ring-3 app with no network and no files.
user/windgate.o: user/windgate.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/windgate.bin: user/windgate.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/windgate.o user/libjt.a

drivers/user_windgate.h: user/windgate.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/windgate.bin drivers/user_windgate.h user_windgate

kernel/ring3app.o: drivers/user_windgate.h

# Panes (2.11): the cmux-style multiplexer, a second app beside the Terminal that shares its shell engine (user/shellcore.h).
user/panes.o: user/panes.c user/jtsys.h user/libjt/text.h user/shellcore.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/panes.bin: user/panes.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/panes.o user/libjt.a

drivers/user_panes.h: user/panes.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/panes.bin drivers/user_panes.h user_panes

kernel/ring3app.o: drivers/user_panes.h

# Claude (2.14): the front end for Claude Code on the relay machine (tools/claude-relay/relay.py), over SYS_HTTP_POST's JT_POST_CLAUDE.
user/claude.o: user/claude.c user/jtsys.h user/libjt/text.h user/libjt/stdlib.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/claude.bin: user/claude.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/claude.o user/libjt.a

drivers/user_claude.h: user/claude.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/claude.bin drivers/user_claude.h user_claude

kernel/ring3app.o: drivers/user_claude.h

# Mines (Apps after 2.2): Minesweeper, 9x9 with 10 mines.
user/mines.o: user/mines.c user/jtsys.h user/libjt/text.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/mines.bin: user/mines.o user/libjt.a user/note.ld boot/memmap.ld
	$(LD) -m elf_i386 -T user/note.ld --oformat binary -o $@ user/mines.o user/libjt.a

drivers/user_mines.h: user/mines.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/mines.bin drivers/user_mines.h user_mines

kernel/ring3app.o: drivers/user_mines.h

drivers/user_mail.h: user/mail.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/mail.bin drivers/user_mail.h user_mail

drivers/user_weather.h: user/weather.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/weather.bin drivers/user_weather.h user_weather

drivers/user_stocks.h: user/stocks.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/stocks.bin drivers/user_stocks.h user_stocks

drivers/user_activity.h: user/activity.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/activity.bin drivers/user_activity.h user_activity

kernel/kernel.o: drivers/user_hello.h drivers/user_note.h drivers/user_wc.h
drivers/user_fbpoke.h: user/fbpoke.bin tools/gen/gen_user_bin.py
	python3 tools/gen/gen_user_bin.py user/fbpoke.bin drivers/user_fbpoke.h user_fbpoke

kernel/ring3app.o: drivers/user_keyrate.h drivers/user_toroid.h drivers/user_calculator.h drivers/user_quotes.h drivers/user_bookrank.h drivers/user_tonchi.h drivers/user_fieldbook.h drivers/user_clock.h drivers/user_portfolio.h drivers/user_activity.h drivers/user_contacts.h drivers/user_hikko.h drivers/user_reminders.h drivers/user_curbfind.h drivers/user_calendar.h drivers/user_search.h drivers/user_epiphany.h drivers/user_weather.h drivers/user_burrow.h drivers/user_stocks.h drivers/user_mail.h drivers/user_notes.h drivers/user_terminal.h drivers/user_samantha.h drivers/user_fbpoke.h drivers/user_brkpoke.h

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

dotfiles.img:
	./tools/gen/sync_dotfiles.sh

run: kernel.elf dotfiles.img
	qemu-system-i386 -kernel kernel.elf -display cocoa,zoom-to-fit=on -rtc base=localtime -net nic,model=rtl8139 -net user -drive file=dotfiles.img,format=raw,if=ide,index=0

# Same boot as `run`, plus "samantha" on the command line: kmain's
# boot_to_samantha skips the desktop for ring-3 Samantha's window
# (user/samantha.c) the instant the splash clears.
samantha: kernel.elf dotfiles.img
	qemu-system-i386 -kernel kernel.elf -append samantha -display cocoa,zoom-to-fit=on -rtc base=localtime -net nic,model=rtl8139 -net user -drive file=dotfiles.img,format=raw,if=ide,index=0

# v1.6.23: `run` plus a real Sound Blaster wired to this Mac's default
# input/output through QEMU's coreaudio backend -- the Yeti (or whatever
# the Mac's default mic is) reaches the guest's SB16 speaker output too,
# not just recording, so Samantha's speak_text plays over real speakers here
# instead of the silent no-op `run` gets without a card at all. See
# user/samantha.c's push-to-talk for why holding F2 records nothing under
# plain QEMU emulation today (QEMU's own -device sb16 has no ADC/record
# path, confirmed against its source) -- coreaudio's "in" side plumbs a
# real host mic into the DSP's input port, so `talk` is still the right
# target for the day QEMU (or a swap to a card QEMU emulates more fully)
# closes that gap, and for real hardware, which this driver is written to.
talk: kernel.elf dotfiles.img
	qemu-system-i386 -kernel kernel.elf -display cocoa,zoom-to-fit=on -rtc base=localtime -net nic,model=rtl8139 -net user -drive file=dotfiles.img,format=raw,if=ide,index=0 \
		-audiodev coreaudio,id=snd0 -device sb16,audiodev=snd0

clean:
	rm -f user/music.o user/music.bin drivers/user_music.h user/libjt/wav.o
	rm -f $(OBJS) $(OBJS:.o=.d) kernel.elf kernel.elf.pass1 kernel/symtab.c kernel/symtab_stub.o kernel/symtab_stub.d user/hello.o user/hello.bin drivers/user_hello.h \
	      user/note.o user/note.bin drivers/user_note.h user/keyrate.o user/keyrate.bin drivers/user_keyrate.h user/toroid.o user/toroid.bin drivers/user_toroid.h \
	      user/calculator.o user/calculator.bin drivers/user_calculator.h user/quotes.o user/quotes.bin drivers/user_quotes.h user/bookrank.o user/bookrank.bin drivers/user_bookrank.h user/tonchi.o user/tonchi.bin drivers/user_tonchi.h user/fieldbook.o user/fieldbook.bin drivers/user_fieldbook.h user/clock.o user/clock.bin drivers/user_clock.h user/portfolio.o user/portfolio.bin drivers/user_portfolio.h user/activity.o user/activity.bin drivers/user_activity.h user/contacts.o user/contacts.bin drivers/user_contacts.h user/reminders.o user/reminders.bin drivers/user_reminders.h user/curbfind.o user/curbfind.bin drivers/user_curbfind.h user/search.o user/search.bin drivers/user_search.h user/epiphany.o user/epiphany.bin drivers/user_epiphany.h user/weather.o user/weather.bin drivers/user_weather.h user/burrow.o user/burrow.bin drivers/user_burrow.h user/stocks.o user/stocks.bin drivers/user_stocks.h user/mail.o user/mail.bin drivers/user_mail.h user/notes.o user/notes.bin drivers/user_notes.h user/terminal.o user/terminal.bin drivers/user_terminal.h user/samantha.o user/samantha.bin drivers/user_samantha.h user/hikko.o user/hikko.bin drivers/user_hikko.h user/calendar.o user/calendar.bin drivers/user_calendar.h
	rm -f user/movies.o user/avi.o user/movies.bin drivers/user_movies.h
	rm -f user/hamurabi.o user/hamurabi.bin drivers/user_hamurabi.h
	rm -f user/windgate.o user/windgate.bin drivers/user_windgate.h
	rm -f user/panes.o user/panes.bin drivers/user_panes.h
	rm -f user/claude.o user/claude.bin drivers/user_claude.h
	rm -f user/mines.o user/mines.bin drivers/user_mines.h
	rm -f joshuatree.iso boot/memmap.ld
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

.PHONY: run samantha clean hooks iso
