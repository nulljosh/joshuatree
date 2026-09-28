#!/usr/bin/env python3
"""The kernel's memory keeps a safe distance from the ring-3 program window.

boot/linker.ld already refuses to link if .bss grows into .userimg, but it
says nothing while the gap is thin, and the gap depends on the toolchain:
PR 239 linked on macOS with 4KB to spare and overflowed on GitHub's Linux
runners. This fails at under 16KB, so a growing feature shows up here,
locally, long before the linker breaks CI.

Reads section headers straight from kernel.elf (ELF32), no tools needed.
"""
import struct, sys

MIN_GAP = 16 * 1024
path = sys.argv[1] if len(sys.argv) > 1 else "kernel.elf"
data = open(path, "rb").read()
if data[:4] != b"\x7fELF" or data[4] != 1:
    sys.exit(f"FAIL: {path} is not a 32-bit ELF")
shoff, = struct.unpack_from("<I", data, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
secs = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
strtab = secs[shstrndx][4]
name = lambda s: data[strtab + s[0]:data.index(b"\0", strtab + s[0])].decode()
by = {name(s): s for s in secs}
if ".bss" not in by or ".userimg" not in by:
    sys.exit("FAIL: kernel.elf has no .bss or no .userimg section")
bss_end = by[".bss"][3] + by[".bss"][5]
user = by[".userimg"][3]
gap = user - bss_end
print(f"bss ends 0x{bss_end:08X}, ring-3 window at 0x{user:08X}: {gap} bytes ({gap // 1024}KB) free")
if gap < MIN_GAP:
    sys.exit(f"FAIL: under {MIN_GAP // 1024}KB between kernel memory and the program window. "
             "Move big static buffers to the heap (see chat_face.h's fingerprints) or move JT_USER_BASE "
             "(kernel/exec.h, user/*.ld, boot/linker.ld) with a real margin.")
print("PASS: kernel memory has room to grow")
