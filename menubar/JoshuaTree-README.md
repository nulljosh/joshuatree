# JoshuaTree.app

A real macOS app bundle, not a decoration: launching the actual kernel via
`open menubar/JoshuaTree.app` (or double-clicking it in Finder) instead of
running `qemu-system-i386` raw gives the running kernel its own real name
and icon in the Dock ("Joshua Tree", the real logo) instead of showing up
as generic "qemu-system-i386".

`Contents/MacOS/JoshuaTree` execs the same real QEMU invocation `make run`
uses (kept in sync by hand, the Makefile is the source of truth for how
this kernel actually boots).

Run `./build-app-icon.sh` after changing `landing/icon.svg` to regenerate
`Contents/Resources/icon.icns` and keep the Dock icon in sync.

Separate from `JoshuaTreeMonitor.app` in this same directory, which shows
build progress from `roadmap.md`, not the kernel itself.
