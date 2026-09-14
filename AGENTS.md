# Joshua Tree

Freestanding i386 kernel, built on macOS with Apple clang and `ld.lld`. No libc or cross-toolchain. Read `CLAUDE.md` for project history, `docs/ARCHITECTURE.md` for the subsystem map, and `roadmap.md` for the current queue.

Build with `make kernel.elf`. Run `./check.sh` for the boot smoke check. It only proves that the kernel reached the GUI; verify changed features against a real QEMU run or a relevant existing harness. `guitest.sh` has a documented QEMU monitor mouse-injection limitation, so do not treat its click results as proof of a GUI bug or fix.

The real Mac app is `menubar/JoshuaTree.app`. Its launcher builds `kernel.elf` and execs QEMU. `./reload.sh` rebuilds and reopens it. The website's v86 demo is separate; only change `landing/v86/embed.js` for a browser-demo issue. If kernel changes affect that demo, copy the rebuilt `kernel.elf` to `landing/v86/kernel.elf` after validation.

`sync_dotfiles.sh` copies only an explicit list of tracked, non-secret config files from the sibling `dotfiles` repo into a gitignored FAT16 image, `dotfiles.img`. The Mac launcher refreshes it before boot; `make run` creates it if missing. Joshua Tree can read these files via its terminal, but cannot execute Fish, Ghostty, cmux, or Starship.

The GUI uses a 960x540 logical window at 2x physical resolution. Dock geometry and interactions are in `kernel/kernel.c`; mouse packets are in `drivers/mouse.c`. The wallpaper wind redraws many pixels and is performance-sensitive. Measure on QEMU before claiming an animation is smooth.

Keep changes narrow. Preserve the higher-half address and DMA physical-address rules. For paging, privilege, interrupt-frame, or wire-format changes, boot success alone is inadequate verification. Use `VERSION` for the current version; old roadmap labels and git tags are historical.
