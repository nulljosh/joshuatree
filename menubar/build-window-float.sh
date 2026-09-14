#!/bin/bash
# Compiles window-float.swift into a real binary the JoshuaTree.app
# launcher execs alongside qemu, only when the source is newer, so a
# normal launch doesn't pay a swiftc cost every time (kernel.elf's own
# rebuild-every-launch is fine since clang is fast on one C file; swiftc
# is not, worth the incremental check here).
set -e
cd "$(dirname "$0")"
if [ ! -x window-float ] || [ window-float.swift -nt window-float ]; then
    swiftc -O window-float.swift -o window-float
fi
