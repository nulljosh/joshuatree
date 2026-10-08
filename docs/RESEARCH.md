# How Joshua Tree compares

**From memory, not checked online.** Written without web access, from what the author remembers of each project. Treat every line as a lead to verify, not a fact. No numbers about other projects are given on purpose.

## The projects

One line each: what it does well that Joshua Tree does not yet.

- **xv6** (MIT teaching kernel): a short companion book explains every line, and a `usertests` program throws bad arguments and edge cases at every system call.
- **SerenityOS**: a full desktop with its own browser engine, a ports tree that builds outside programs against its libc, man pages with a Help app, and unit tests run on every change.
- **Redox** (Rust): memory-safe kernel and drivers, drivers run as user programs outside the kernel, and a package manager with a real package format.
- **seL4**: a machine-checked proof that the kernel matches its spec, and capabilities as the only way to reach anything.
- **Haiku**: a mature filesystem with file attributes and live queries, a package manager whose packages mount in place, and broad real hardware support including USB and networking.
- **ToaruOS**: its own dynamic linker and shared libraries, a compositing window manager, and a ported Python.
- **Circle** (bare-metal C++ for the Pi): drivers for the Pi's USB, SD card with FAT, sound, I2C and SPI, and multi-core, used as a library by other bare-metal projects.
- **Ultibo** (bare-metal Pascal for the Pi): a whole Pi environment with USB, networking, filesystems and multi-core, plus an IDE that builds the Pi image.
- **MenuetOS**: a graphical OS written in assembly that boots very fast from tiny media, with networking built in.
- **TempleOS**: one language (HolyC) is both the shell and the compiler, so you edit and run any part of the OS from inside it, and the whole source is meant to be read.
- **AI-assistant-first efforts** (research "LLM as an operating system" designs such as AIOS and MemGPT, and assistant devices with their own OS): they schedule and give memory to agent tasks as first-class jobs; none of them run on a from-scratch kernel, which is where Joshua Tree differs.

## Already on our roadmap

These came up for several projects above and are already planned in `docs/roadmap.md`, so they are not repeated below: SD card writes with MBR and FAT32, USB mass storage, the `get` installer, all four Pi cores, TCP that survives a lost segment, Bluetooth, a journaling filesystem, W^X and address randomising, a crash reporter, the plain-words guide, a compiler on the box, and mruby.

## Gaps worth bridging

Ranked by what it buys for the effort. Each names the check that would prove it.

1. **Bad-argument tests for every system call** (xv6 `usertests`). Size: medium. Check: a ring-3 program passes bad pointers, huge lengths and closed handles to each call, and the kernel returns an error for each and keeps running; it fails if a bound check is removed. (`usertest` proves the ABI works and the ring-3 stress check proves the heap survives load; neither feeds bad arguments.)
2. **Attach a debugger to QEMU** (every project above uses QEMU's gdb stub). Size: small. Check: a script boots both ports paused, attaches lldb (it ships with the Mac toolchain), stops at the kernel entry and reads a register.
3. **A manual page for every shell command** (SerenityOS, Unix). Size: small. Check: `man NAME` prints a page, and a check fails if any command in the shell's table has no page.
4. **A ports folder** (SerenityOS, ToaruOS): build scripts that compile outside C programs against libjt, Lua first. Size: medium. Check: the ported Lua runs a script as a ring-3 app and prints the right answer.
5. **One driver out of the kernel** (Redox, seL4): the mouse as a ring-3 program. Size: large. Check: kill the mouse program, the kernel keeps running, restarts it, and the pointer moves again.
6. **The syscall page checked against the code** (in the spirit of seL4's spec, much smaller). Size: small. Check: every number and name in the system call table of `docs/SYSCALL-ABI.md` matches the kernel's dispatch table.
7. **Shared libraries** (ToaruOS). Size: large. Check: two ring-3 apps load one copy of libjt and both run.
8. **IPv6** (Haiku, SerenityOS). Size: medium. Check: the ARM stack gets an address on a QEMU IPv6 network and answers a ping.
