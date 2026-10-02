# Joshua Tree syscall ABI

This is the contract a ring-3 program written against this kernel can rely
on. It exists because of what 1.0.0 has to mean here: semver's 1.0.0 claims
a stable public interface, and until there was a real external caller there
was nothing for a MAJOR version to protect. This file is that interface.

Three versions are shipped. **v1 is frozen**: every number, argument meaning
and error below in the v1 section works exactly as written and will not
change without a MAJOR bump. **v2 adds** file writing, a real seek, and
command-line arguments. v2 adds numbers, flag bits and one loader
convention; it redefines nothing v1 said. `user/hello.c` is unchanged and
`tools/checks/usertest-check.sh` still runs it unmodified. **v3 adds** two
window calls so a program can be a desktop app, the first step of 2.0's
"apps leave the kernel"; see the v3 section at the end.

# v1, frozen

The whole v1 set is now implemented, and `user/hello.c` is the real
external caller. It is compiled separately, with no kernel include path at
all, against `user/jtsys.h` and nothing else. It is linked as a flat
binary, written to the VFS, read back off the VFS, and run as a genuine
ring-3 task. `tools/checks/usertest-check.sh` runs it on every push and
asserts its actual output and the exit code the kernel observed, so this
document is checkable rather than merely written down.

## Calling convention

Borrowed from x86 Linux rather than invented, so anything written against
that convention lines up here without translation:

- `int $0x80` is the gate. IDT vector 0x80, DPL 3, 32-bit interrupt gate,
  so interrupts are off for the duration of the call.
- `eax` holds the syscall number on entry and the result on return.
- `ebx`, `ecx`, `edx` hold arguments 1 through 3. There is no fourth
  argument in v1, and adding one later does not break this contract.
- A negative return value is `-errno`. Errno values are Linux's own.
- Every unassigned number returns `-ENOSYS` (38), including every number
  at or above the dispatch table's size. Dispatch never jumps into an
  empty table slot.
- The call runs on the calling task's own kernel stack, repointed by the
  TSS `esp0` that `task.c`'s scheduler updates on every switch.

Any pointer a program hands the kernel is checked against the page tables
before the kernel reads or writes it, the job Linux's `access_ok` does. An
unmapped or non-user-accessible range gets `-EFAULT` (14), never a kernel
read of a kernel address on a user program's behalf. A path string is
checked one byte at a time as it is copied, because its length is not
known until the terminating NUL is found.

## The v1 call set

Numbers are Linux i386's own, so `__NR_exit` is 1 and `__NR_write` is 4.
The gaps are deliberate, not slots waiting to be filled in order.

| # | Call | Arguments | Returns | Status |
|---|------|-----------|---------|--------|
| 1 | `exit` | ebx = exit code | never returns | shipped |
| 3 | `read` | ebx = fd, ecx = buf, edx = len | bytes read, 0 at end of file | shipped |
| 4 | `write` | ebx = fd, ecx = buf, edx = len | bytes written | shipped |
| 5 | `open` | ebx = path, ecx = flags | fd, or -errno | shipped |
| 6 | `close` | ebx = fd | 0, or -errno | shipped |
| 13 | `time` | ebx = 0, or an `unsigned *` | seconds since the epoch | shipped |
| 20 | `getpid` | none | this task's id | shipped |
| 158 | `sched_yield` | none | 0 | shipped |

File descriptors 0, 1 and 2 are standard input, output and error. There is
one console, so 1 and 2 both reach it. `read` on fd 0 reads the keyboard.
Descriptors from `open` start at 3 and are per task.

## The limits, exactly

Every number here is part of the contract. They are the values the
implementation actually uses, not round numbers chosen for the document.

- `write` copies at most **255** bytes per call, so the kernel-side copy
  has a fixed on-stack buffer. It also stops at the first NUL byte, so it
  writes text, not arbitrary binary. Longer output is more calls.
- `read` copies at most **255** bytes per call, the same bound for the
  same reason.
- `open` accepts a path of at most **63** bytes plus the terminating NUL.
  A longer one, or one with no NUL in range, is `-EINVAL` (22).
- `open` reads the whole file eagerly into a kernel buffer and `read`
  serves out of that snapshot, because `vfs_read_file()` is the only read
  primitive the VFS layer has: there is no seek and no partial read
  underneath. Two consequences, both real: a file larger than **8192**
  bytes cannot be opened at all (`-EINVAL`, never a silent truncation),
  and a file that changes on disk after `open` is not seen by an fd that
  is already open.
- Each task has **8** descriptor slots. Three are the standard streams, so
  **5 files can be open at once** per task, numbered 3 through 7. A sixth
  is `-EMFILE` (24).
- `open`'s flags argument must be **0**. v1 is read-only; any other value
  is `-EINVAL`. There is no create, no write, no append.
- Descriptors are per task slot and are closed for you when the task
  exits, including when it exits by faulting.
- `read` on fd 0 is **non-blocking**, and this is forced, not chosen:
  `int $0x80` is an interrupt gate, so interrupts are off inside the call
  and the keyboard IRQ that would deliver the awaited byte cannot fire.
  Blocking would hang the machine. With nothing queued it returns
  `-EAGAIN` (11); a program that wants to wait calls `sched_yield` and
  reads again.
- `read` on fd 1 or 2 is `-EBADF` (9). The console is write-only.
- `time` returns seconds since 1970-01-01T00:00:00Z, read from the CMOS
  RTC. If `ebx` is non-null it is also written through, after the usual
  pointer check; the value is returned in `eax` either way.
- `getpid` returns the task's scheduler slot id. The shell is 0, so a
  ring-3 program started from it is never 0. There are **6** slots in
  total, so ids are 0 through 5.
- The dispatch table has **160** entries (the highest v1 number, 158,
  rounded up to the next multiple of 32). This is an implementation
  detail with no contractual meaning: every number not in the table above
  returns `-ENOSYS` whether it is inside that range or above it.

## How a program is built and loaded

Also part of the contract, because a flat binary has no headers and no
relocations, so its load address is not negotiable:

- A user program is a flat binary. Entry is offset 0, not an ELF entry
  point; `exec_user()` jumps straight at the load address.
- It is linked at **0xC0587000** and gets **33 pages**: thirty-two for the
  image (128KB, `JT_USER_IMAGE_PAGES` in kernel/memmap.h; a larger one
  fails to link), one for its stack, whose top, 0xC05A8000, is the initial
  `esp`. (Until 2026-10-01 the image was 7 pages, 28KB, and the stack top
  0xC0588000; the base address did not move.) (v2 pushes the argument block
  onto that page, so the initial `esp` is now a little below the top; see
  "One thing v1 said that v2 makes less than literally true" below. The
  page, its top and its size are unchanged.) `boot/linker.ld` reserves that
  window so the physical memory manager never hands those frames to the
  heap, and `exec_user()` marks exactly those eight pages user-accessible
  and zeroes them before loading, so nothing of the previous program
  survives into the next one. The address appears in three files by
  necessity (`kernel/exec.h`, `user/hello.ld`, `boot/linker.ld`) and
  `tools/checks/usertest-check.sh` fails if they ever disagree.
- It gets no `.bss`: with a flat image there is nothing to zero it and no
  crt0 to do the zeroing, so `user/hello.ld` refuses to link one.
- One program runs at a time. There is one image window, and the shell
  waits for the program it started.
- It runs with IOPL 0 in its own page directory, so any `in`/`out` it
  tries is a `#GP` and it cannot see another task's private pages.

## What this contract does NOT cover

Honest scope, so nothing here overclaims:

- No memory-mapping call, no `brk`, no `mmap`. A v1 program gets the
  eight pages it was loaded with, and that is all the memory it will ever
  have.
- No `fork` or `exec`. Tasks are created by the kernel, not by user code.
- No signals, no IPC, no sockets. The network stack is kernel-side only.
- No directory enumeration, no seek, no stat. `open`/`read`/`close` on a
  known path only.
- No writing to files. `open` is read-only in v1.
- No command-line arguments and no environment. `_start` takes nothing.
- Nothing about the GUI. Every app you can click is kernel code today, not
  a ring-3 program, and converting them is a separate, larger job.

# v2, shipped

v1 shipped an interface a program could look through and not change
anything with: `open` demanded `flags == 0` and `write` only reached the
console. v2 is the other half. It is additive, and the additions are
listed here with the same "every number is the real one" rigor v1 got.

## The v2 call set

| # | Call | Arguments | Returns | Status |
|---|------|-----------|---------|--------|
| 4 | `write` | ebx = fd **3..7**, ecx = buf, edx = len | bytes written | shipped |
| 5 | `open` | ebx = path, ecx = **flags** | fd, or -errno | shipped |
| 19 | `lseek` | ebx = fd, ecx = offset, edx = whence | the new position, or -errno | shipped |

`write` and `open` are the same numbers with the same arguments as v1. What
changed is only that values v1 rejected are now accepted: an fd from
`open`, and a non-zero flag word. Every v1 call still returns what v1 said
it returns.

## open flags

Linux i386's own values, the same borrow the call numbers are. The access
mode is the low two bits; the rest are independent.

| Flag | Value | Meaning |
|---|---|---|
| `O_RDONLY` | 0 | read only. This is v1's `flags == 0`, unchanged |
| `O_WRONLY` | 0x001 | write only; `read` on this descriptor is `-EBADF` |
| `O_RDWR` | 0x002 | both |
| `O_CREAT` | 0x040 | create the file if it does not exist |
| `O_TRUNC` | 0x200 | start from an empty file |
| `O_APPEND` | 0x400 | every `write` goes to the end, whatever the offset is |

- Access mode `3` is not a mode and is `-EINVAL`.
- Any bit outside the table is `-EINVAL`, not ignored. A program is never
  told a request succeeded when part of it was dropped.
- `O_CREAT`, `O_TRUNC` and `O_APPEND` require `O_WRONLY` or `O_RDWR`.
  Asking for one read-only is `-EINVAL`, because nothing is ever written
  back from a read-only descriptor and a silent no-op would be worse.
- `O_CREAT` and `O_TRUNC` take effect **at open**: the file exists, and is
  empty, the moment `open` returns. Only the bytes a program goes on to
  write are deferred (see below).
- `O_CREAT` on a backend with no room left is `-ENOSPC` (28).

## How writing actually works, and what that costs

This is the part to read before writing a program against it, because the
model is not POSIX's and the difference is visible.

The VFS layer underneath has exactly two write primitives,
`vfs_write_file()` and `vfs_replace_file()`, and **both take a whole file at
once**. There is no partial write, no truncate-to-length, no per-backend
cursor. So a writable descriptor is v1's read snapshot run backwards: the
file is loaded whole into one kernel buffer at `open`, `write` edits that
buffer at the descriptor's own offset, and `close` hands the whole buffer
back through `vfs_replace_file()`. Nothing else the VFS offers could
implement `write` honestly.

The consequences, all real and all contractual:

- **The file on the filesystem does not change until `close`.** A program
  that writes and then exits without closing is still fine: task exit
  closes every descriptor, and that flush happens, however the task ended,
  including by faulting.
- **Two descriptors open on the same file do not see each other**, and the
  last one closed wins the whole file. There is no locking.
- **`close` can fail.** It returns `-EIO` (5) if the bytes did not reach
  the backend intact, and that is a real check, not a formality: `close`
  reads the file straight back and compares it to what it just wrote.
  That is necessary because `vfs_replace_file()` answers 1 or 0 and
  nothing else, and the ramfs backend silently truncates anything past its
  own 4096-byte per-file cap while still answering 1. Check `close`'s
  return value. It is the call that stores your data.
- **The descriptor is released even when `close` returns `-EIO`.** A failed
  close has still closed.

## The limits, exactly

Again, the values the implementation uses, not round numbers.

- `write` to a file copies at most **255** bytes per call, the same bound
  as v1's console `write` and for the same reason.
- `write` to a file is **binary clean**: unlike `write` to fd 1 or 2, it
  does not stop at a NUL byte. The inconsistency is deliberate. Changing
  what a v1 write to the console does would be a MAJOR break for no gain,
  and a descriptor from `open` has no v1 behaviour to preserve.
- A file cannot grow past **8192** bytes, `OPEN_MAX_FILE`, the same ceiling
  v1 put on reading. A `write` that would cross it is `-EFBIG` (27) and
  writes **nothing**, never a short write the caller has to notice.
- `write` on a descriptor opened `O_RDONLY` is `-EBADF` (9). `read` on one
  opened `O_WRONLY` is `-EBADF`.
- `write` to fd 0 is `-EBADF`, as in v1.
- `lseek` whence is `SEEK_SET` (0), `SEEK_CUR` (1), `SEEK_END` (2). Anything
  else is `-EINVAL`.
- `lseek` to exactly the file's current size is legal: that is end of file,
  where an append lands. **Past it is `-EINVAL`**, not a hole. A hole would
  mean inventing zero bytes the program never wrote.
- `lseek` on fd 0, 1 or 2 is `-ESPIPE` (29). Those are streams, not files.
- `lseek` offsets are signed 32-bit, which is not a limit worth worrying
  about when a file caps at 8192 bytes.
- A **zero-length file and a missing file are the same thing** through this
  interface. The backend read primitive returns a byte count and nothing
  else, so there is no answer that distinguishes them. v1 resolved that as
  "missing" (`-ENOENT`) and v2 keeps it. In practice this means `O_CREAT`
  on an existing empty file re-creates it, which is harmless, and that a
  file you truncated to nothing reads back as not existing.

## Command-line arguments

A program is entered with the i386 cdecl frame a plain C function already
expects, so this is a valid entry point with no assembly anywhere:

```c
void _start(int argc, char **argv) { ... jt_exit(0); }
```

Exactly, at the moment the CPU lands on the entry point:

```
esp+0   a return address, always 0
esp+4   argc
esp+8   argv  ->  [ argv[0], ..., argv[argc-1], NULL ]
...     the argv pointer array
...     the argument strings, NUL-terminated
top     0xC0588000, the top of the program's stack page
```

Why this layout and not Linux's: real Linux puts `argc` at `0(%esp)` with
no return address above it, which is exactly why a real i386 crt0 has to be
written in assembly. There is no crt0 here at all, so the loader picks the
layout the compiler already generates code for and the C source stays C.

The rest of the contract around it:

- `argv[0]` is the program's own name by convention. The shell's `exec`
  puts the filename there. Nothing in the kernel invents one.
- `argv[argc]` is `NULL`, so argv can be walked without argc.
- The strings and the pointer array live at the top of the program's own
  stack page, which the loader zeroes and marks user-accessible before the
  program starts. One program's arguments cannot be read out of that page
  by the next one.
- At most **8** arguments including `argv[0]`, and at most **256** bytes of
  argument text including the NULs. Past either, the exec fails and the
  program never starts, rather than starting with a quietly shortened argv
  it has no way to detect.
- A program that wants nothing from its arguments can still declare
  `void _start(void)`. `user/hello.c` does, which is why it did not have to
  change for v2.
- **`_start` must never return.** There is no caller above it and the
  return address is 0, so returning faults at an unmapped address. The
  kernel reaps the task the same way it reaps any faulting program, but the
  exit status is the fault path's, not yours. Call `jt_exit`.

## One thing v1 said that v2 makes less than literally true

Flagged rather than buried, because v1 is frozen and this is the one place
the frozen text and the running kernel no longer read the same.

v1's "How a program is built and loaded" says the stack page's top,
0xC0588000, is the initial `esp`. With arguments on the stack that is no
longer exact: `esp` starts below the argument block, by twelve bytes plus
the pointer array plus the strings, and at most 256 + 8*4 + 12 bytes below
the top in the worst case v2's own limits allow.

Why this is not treated as a MAJOR break:

- The stack page itself, its address, its top and its size are all
  unchanged, and it is still the only stack a program gets.
- No v1 program can observe the difference through the v1 interface.
  `user/jtsys.h` exposes no stack pointer, and a v1 `_start(void)` is
  defined as taking nothing, so there is nothing above `esp` it is entitled
  to read. A program that read its own `esp` and compared it to a literal
  0xC0588000 would notice, and no such program exists or could have been
  written usefully.
- `user/hello.c` is unmodified and `tools/checks/usertest-check.sh` passes
  unmodified, which is the practical version of the same claim.

If that is not good enough for some future caller, the fix is a MAJOR bump,
not a quiet edit to the v1 section above.

## The v2 reference program

`user/note.c`, built and loaded exactly like `user/hello.c` and against
`user/jtsys.h` and no kernel header. The difference is that it is a program
someone would run rather than an interface self-test:

```
note FILE              print it
note FILE text...      append a line, creating the file
note FILE @N text...   overwrite the bytes at offset N
```

The third form is what makes `lseek` load-bearing rather than decorative:
the descriptor is opened for writing without `O_TRUNC`, so it still holds
the file's existing bytes, the seek moves into the middle of them, and the
write replaces exactly what it covers.

`tools/checks/notetest-check.sh` runs it on every push. Its strongest
assertion is not one of the program's printed lines: after the runs, the
kernel reads the file back through the VFS itself and compares the bytes,
because everything the program prints is the program's claim about what it
did, and a write path that printed the right numbers and stored nothing
would pass the first check and fail that one.

## What v2 still does NOT cover

Honest scope again, and the list is still long.

- **One program at a time, still.** There is one image window and one user
  stack page, and the shell blocks on the program it started. There is no
  `fork`, no `wait`, no job control, no way for two user programs to be
  alive at once.
- ~~No shell that runs programs by name.~~ Fixed for 1.0.0: an unknown
  command now falls through to the same lookup `exec` uses
  (`exec_resolve_name()` in `kernel/exec.c`), case-insensitive and trying
  the real on-disk extension, before the shell reports it unknown. `exec
  NOTE.BIN args...` still works exactly as before; typing `note buy milk`
  does too. The kernel shell is still an `if`-ladder of built-ins, and
  built-ins still win over a program of the same name -- there is no
  `$PATH`, just one lookup against the active VFS backend.
- **No `unlink`, no `rename`, no `mkdir`, no directory enumeration, no
  `stat`.** A program can read and write files it already knows the names
  of. Removing one is still a kernel-side `rm`.
- **No append-without-reading.** Opening a file for writing reads the whole
  thing first, because the write-back model needs it. Appending one line to
  an 8KB file moves 8KB twice.
- **No `dup`, no redirection, no pipes.** fd 1 is the console and nothing
  can change that.
- **Still no `brk`/`mmap`, no signals, no IPC, no sockets.** A program gets
  the eight pages it was loaded with, and that is all the memory it will
  ever have.
- **Nothing about the GUI.** Every app you can click is still kernel code,
  not a ring-3 program.

## What a MAJOR version means, from here on

Changing the meaning of a number in the table above, removing a call,
changing an argument's meaning, or tightening one of the limits in "The
limits, exactly" is a breaking change and requires a MAJOR bump. Adding a
new number, adding a flag, raising a limit, or relaxing an error case is
not.

Before this, the file described an interface nothing depended on. Now two
real reference programs are built against it and run by the regression
suite on every change, which is what makes the promise checkable rather
than stated: `user/hello.c` for v1 and `user/note.c` for v2.

## Writing a program with libjt

`user/hello.c` and `user/note.c` are written straight against `jtsys.h`'s
`int $0x80` wrappers, on purpose: they are the interface's own self-test
and should look exactly like the ABI they exercise. A program that just
wants to get something done doesn't have to write at that level -- it can
link `user/libjt/`, a small C library the Makefile builds into
`user/libjt.a` (`llvm-ar`) with the same freestanding flags as everything
else in `user/`.

What it gets: `string.h` (`strlen`, `strcmp`, `strcpy`, `memcpy`, and the
rest of the usual set), `ctype.h`, `stdlib.h` (`atoi`, `strtol`, `abs`,
`exit`, and `malloc`/`calloc`/`realloc`/`free` over a fixed 16KB static
arena, since there is still no `brk`/`mmap` -- that arena's size is the
real ceiling on how much a libjt program can allocate, not "out of RAM"),
and `stdio.h` (`printf`/`snprintf`/`vsnprintf` with `%d %i %u %x %X %c %s
%p %%`, width and zero-padding; a minimal `FILE` with `fopen`/`fread`/
`fwrite`/`fgets`/`fclose` over the same syscalls; `stdin`/`stdout`/
`stderr`).

To build one: write a `_start` the way `user/note.c` does (`void
_start(int argc, char **argv)`, or `void _start(void)` if you don't need
argv), `#include` whichever libjt headers you need, and link
`user/libjt.a` in after your own object file -- see `user/wc.c` and its
Makefile rule (`user/wc.bin`) for the pattern. Everything "What this
contract does NOT cover" says above still applies: no `brk`, no `fork`,
one program running at a time, the same 32-page image ceiling. libjt does
not get around any of that, it just saves you from re-writing `strlen`
and a decimal formatter in every program that needs one.

# v3, shipped (1.7.7)

Two calls, so a ring-3 program can be an app on the desktop rather than a
line of text in the shell. They are Joshua Tree's own numbers, **384 and
385**, starting past anything Linux i386 assigns so the two sets can never
collide. Register shape is unchanged: `eax` number, `ebx`/`ecx`/`edx`
arguments, result in `eax`, negative errno on failure. Every user pointer
is checked the way every v1 and v2 pointer is. Nothing v1 or v2 said
changes.

## The v3 call set

| # | Name | ebx | ecx | Returns |
|---|---|---|---|---|
| 384 | `window_open` | `struct jt_window_info *` | 0 | 0, or -errno |
| 385 | `window_poll` | `struct jt_event *` | flags | 1 with an event written, -EAGAIN with none, or -errno |

```c
struct jt_window_info { unsigned int width, height, pitch; unsigned int *pixels; };
struct jt_event { unsigned int kind; int a, b; };
```

**window_open** hands the program the app window the desktop already
opened for it. There is no "create a window at x,y" here on purpose: the
desktop draws the chrome and sets the viewport exactly as it does for an
in-kernel app (see `gui_launch_from_dock` in `kernel/kernel.c`), then the
launcher (`kernel/ring3app.c`) runs the program, and the program asks for
that viewport. It gets its size in `width`/`height`, `pitch` in bytes
(always `width * 4`), and `pixels`, a framebuffer of exactly that size,
32 bits per pixel, `0x00RRGGBB`, mapped user-accessible at a fixed address
(`JT_USER_FB`, 0xC05A0000, reserved by `boot/linker.ld`). The program
draws into it directly. Errors: -EFAULT (bad pointer), -EBUSY (another
program owns the window), -ENODEV (no app viewport is open, e.g. the
program was run from the text shell), -ENOMEM (viewport bigger than the
buffer, which the dock's window never is).

**window_poll** is how pixels reach the screen and how input reaches the
program, and it never blocks: `int 0x80` runs with interrupts off, so the
kernel cannot sleep a program inside it. With `JT_POLL_PRESENT` (1) in
`ecx` the kernel first copies the framebuffer into the viewport through
the same `window_pixel` path every in-kernel app draws with (so clipping,
the back buffer and window drag come for free), then looks once at the
keyboard and mouse. One event, or -EAGAIN; the program yields and asks
again. Any other bit in `ecx` is -EINVAL. Event kinds:

| kind | meaning | a | b |
|---|---|---|---|
| 1 `JT_EV_KEY` | a key | ASCII, or 256 and up for up/down/enter/esc/left/right, the same values `kernel/app.h` gives in-kernel apps | 0 |
| 2 `JT_EV_CLICK` | a left click | x in window coordinates | y |
| 3 `JT_EV_WHEEL` | a wheel tick | +1 up, -1 down | 0 |

There is no `window_close`. Exiting releases the window; so does
crashing. `task_exit_with` runs `syscall_release_task` for a task that
faulted exactly as for one that called `exit`, and the window is torn
down there, so the launcher never waits on a program that is gone.

## What v3 guarantees, and what it does not

The point of v3 is isolation, and this is the part `tools/checks/
ring3app-check.py` proves rather than states: a program with a window is
still an ordinary ring-3 task. It runs at CPL 3, IOPL 0, with its own page
directory, and touches the machine only through these numbers and its own
framebuffer pages. Writing through a null pointer inside Keyrate is a page
fault the kernel reaps; the window goes away; the desktop repaints and
takes the next click. Nothing in ring 0 stops.

Limits, exactly: one program window at a time (`-EBUSY` otherwise), the
same one-program-at-a-time rule as `exec_user`. The framebuffer is
`0x110000` bytes, enough for the dock's 804x345 viewport with a little
room. The kernel copies the whole buffer on every present, so a program
should present only after it drew something. No font: a program draws its
own glyphs (`user/keyrate.c` carries the kernel's 8x16 VGA fallback font
as data). No finer clock than `time`'s seconds.

Release is complete, as of 1.7.8. When the owning task ends, by `exit` or
by a fault, the buffer is zeroed and its pages are flipped back to
supervisor-only (`paging_clear_user`), on every teardown path. The next
program, with no `window_open` of its own, page-faults if it stores into
`0xC05A0000`, and a syscall handed a pointer into that range gets
`-EFAULT`, the same answer as for a pointer into the kernel. 1.7.7 only
zeroed; `tools/checks/userfb-release-check.py` runs `user/fbpoke.c` right
after a window closes and asserts both refusals and the fault.

## The v3 reference program

`user/keyrate.c`, the typing test, launched from the dock by
`kernel/ring3app.c` (which since 1.7.11 also launches `user/toroid.c` from one table)
(which is kept for now). The backquote key makes it write through a null
pointer on purpose; the check presses it and asserts the desktop is still
alive afterwards, with the serial log naming the fault.

## Windows as compositor windows (1.9.23)

The calls did not change. What changed is what they mean for a program the desktop launched on the window path (`kernel/ring3app.c` `ring3app_launch_window`):

- `window_open` fills the same `jt_window_info`; `pixels` is still `JT_USER_FB`, but the frames behind it belong to this task alone, mapped into no other directory.
- `window_poll` with `JT_POLL_PRESENT` no longer copies the buffer to the screen inside the call. It marks the window dirty and the desktop blits it on its next frame, so a program that presents every loop costs the kernel nothing extra.
- Events come from a per-window queue of 16. The desktop puts a key or click there only while this window is focused; a full queue drops the newest. `JT_EV_CLICK` coordinates are relative to the content area, as before.
- A program on the blocking path (every app not named in `gui_ring3_windowed`) sees the old behaviour exactly.

## tasks (1.9.6)

| # | Name | ebx | ecx | Returns |
|---|---|---|---|---|
| 386 | `tasks` | `struct jt_tasks *` | slot to kill, or -1 | 0, or -errno |

```c
struct jt_tasks { unsigned int ticks, free_kb, total_kb, current, used; };
```

**tasks** is what the Activity app shows: uptime in 100Hz ticks, free and total
memory in KB, the caller's own slot, and a bitmask of the live scheduler slots.
If `ecx` names a slot it is killed first through `task_kill`, the same call the
shell's `kill` makes. Slot 0 (the shell) and the caller's own slot are refused
with -EPERM, a free slot is -ENOENT, and the snapshot is filled either way.
Errors: -EFAULT (bad pointer).

## http_get (1.9.11)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 387 | `http_get` | `const char *path` | `void *buf` | `len` (at most 65536, `JT_HTTP_BIG_MAX`) | body bytes, -status, or -errno. Up to 2048 bytes the reply is bounced and copied out only on a 200; above that it is received straight into `buf` (range-checked whole with `paging_user_range_ok`, 3s budget), so a failed fetch may leave scratch bytes there. Samantha fetches ~21KB face JPEGs this way. |

**http_get** is what the Curbfind app fetches its live rows with, and the
first call that puts the kernel's network stack behind a ring-3 program. It
sends one `GET <path> HTTP/1.0` to `joshuatree.heyitsmejosh.com` port 80; the
host and port are fixed in the kernel and a program cannot name another. The
path is copied out of user memory a byte at a time with the same check every
other pointer gets, and is refused with -EINVAL unless it is at most 128 bytes
before the NUL, starts with `/`, and is printable ASCII (0x21 to 0x7E) all the
way: no space, which would end the request line early, and no CR or LF, which
would let a program write a header of its own. `buf` and `len` must lie inside
user memory or the call is -EFAULT; `len` is clamped to 2048, the kernel's
bounce buffer. The reply waits at most 1500ms.

On an HTTP 200 the body is copied out, at most `len` bytes, and the count is
returned. Any other status returns minus that status, -100 to -599, which can
never collide with an errno (all below 100). -ENODEV means no NIC, -EIO no
answer in time, -EBUSY a fetch already in flight from another task. Nothing is
written to `buf` on any failure. Every one of the refusals happens before the
network is touched, so `tools/checks/ring3curbfind-check.py` can prove them on
a guest with no NIC at all: Curbfind's `p` key hands the call a relative path,
a CR LF, a space, an over-long path, a null buffer, a buffer in kernel text and
a path pointer in kernel text, and each must come back -22 or -14 with the
buffer untouched.
## readdir (1.9.13)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 388 | `readdir` | `const char *path` | `struct jt_dirent *` | records the array holds | entries in the directory, or -errno |

```c
struct jt_dirent { char name[32]; unsigned int size, is_dir; };
```

**readdir** is what the Search app lists (and what Files will). `path` is
relative to the shell's current directory: `""` or `"."` is that directory,
`"DOCS"` a folder inside it, `"DOCS/SUB"` two levels down. No leading slash,
no `.` or `..` parts, at most 63 bytes before the NUL, at most 8 parts. The
kernel walks the path one `vfs_chdir` at a time, lists, and walks back with
`..` before returning, all inside the gate with interrupts off, so a program
can never move the kernel's own current directory: it keeps its own cwd
string and passes it each call. One record per entry, fixed size: the name
NUL-terminated and cut to fit, `size` in bytes (0 for a folder), `is_dir`
1 or 0. `edx` is clamped to 64. The return value is the number of entries the
directory holds, which can be more than `edx`: only the first `edx` records
are written, so a caller that gets back more than it asked for knows the
listing was cut. The array is checked whole before anything runs and is not
touched on an error. Errors: -EFAULT (path or array not user memory),
-EINVAL (path too long or malformed), -ENOENT (a part is not a folder, or the
backend has no folders at all, which is ramfs).

Since 1.9.13 **open** takes the same relative paths, so the file a listing
named inside `DOCS` opens as `"DOCS/NAME"`. The walk in and back out happens
at open and again at close, when the buffer is written back. An empty path
is -EINVAL.

## mkdir and unlink (1.9.24)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 389 | `mkdir` | `const char *path` | 0 | 0 | 0, or -errno |
| 390 | `unlink` | `const char *path` | 0 | 0 | 0, or -errno |

**mkdir** makes a folder and **unlink** deletes a file, both for the Notes app.
`path` follows the exact rules of **open**: relative to the shell's directory,
no leading slash, no `.` or `..` parts, at most 63 bytes before the NUL, at
most 8 parts. Every part but the last is entered with `vfs_chdir` and walked
back with `..` before the call returns, with the cursor put back where the
desktop had it, so ring 3 never moves the kernel's own directory. The last
part is the name made or removed. The path is copied out byte by byte, each
one checked with `paging_user_range_ok`; nothing outside what open can reach
is reachable here. No heap is touched, so there is no lock to take beyond the
gate's own interrupts-off. Errors: -EFAULT (path not user memory), -EINVAL
(too long or malformed), -ENOENT (a part is not a folder, the backend has no
folders as with ramfs, or for unlink the file is not there), and for mkdir
-ENOSPC, which covers a taken name, a full disk and a full directory, since
the backend answers only yes or no.

## shell_run (1.9.24)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 391 | `shell_run` | `const char *line` (`"<cwd>\n<command>"`) | `char *out` | `unsigned outlen` | bytes written to `out`, or -errno |

**shell_run** is the one call behind the window Terminal (`user/terminal.c`). It
runs a single shell line and writes the text the shell would have printed into
`out`, NUL terminated and cut to `outlen - 1`. `line` is copied in byte by byte
with each byte checked by `paging_user_range_ok`, at most 160 bytes before the
NUL (`JT_SHELL_LINE_MAX`: a 63 byte cwd, the newline, a 95 byte command); `out` is checked whole, and `outlen` must be 1 to
4096. Errors: -EFAULT (either pointer not user memory), -EINVAL (`outlen` out
of range, line too long). A refused command is not an error: it returns a
one-line message.

It does not call the text shell's `run()`. The call executes inside the int
0x80 gate on the 4KB kernel stack with interrupts off, and `run()` has
4KB buffers in its frame and commands that sleep, wait on the network, open
windows or halt. `kernel/shellsys.c` is a small dispatcher with static buffers
and an explicit allowlist instead, in the text shell's own wording:

| Command | Does |
|---|---|
| `help` | lists the allowlist |
| `echo <text>` | prints the text |
| `uptime` | seconds since boot |
| `mem` | free and total memory in K |
| `ps` | each task slot, used or free |
| `ls [dir]` | the cwd (or `dir` under it), name and size |
| `cat <file>` | a file under the cwd, up to 2047 bytes, non-printable bytes shown as `.` |

Everything else is refused with `<name>: not available in the window terminal
(allowed: ...)`. That covers anything that blocks (`sleep`, `bench`, the
`*test` family), waits on the network (`ifconfig`, `netscan`, `web`, `chat`,
`say`), opens a GUI app or window (`gui`, `browse`, `notes`, `exec`), reboots or
halts, or re-enters the window system.

The working directory belongs to the caller. `line` is `<cwd>\n<command>`: the
Terminal keeps its own cwd as a relative path from the root (empty is the
root, at most 63 bytes, up to `JT_PATH_DEPTH` components) and sends it with
every call. The kernel joins it with the argument of `ls` or `cat` and resolves
it with the same `path_enter` / `path_leave` walk the file syscalls use, so
the desktop's cwd is never moved and is restored exactly before the call
returns. A line with no newline means the root. `cd` never reaches the kernel:
the Terminal validates the target with `readdir` (-ENOENT or a file means "no
such folder"), then updates its own string; `cd ..` pops a component and `cd`
or `cd /` returns to the root. `clear` is local too. A cwd or path that does
not resolve answers `ls: no such folder` or `<file>: not found`.

Since 1.9.24 the ring-3 key paths also deliver Home, End, Delete (0xE0 0x47,
0x4F, 0x53) as `JT_KEY_HOME` 305, `JT_KEY_END` 306, `JT_KEY_DELETE` 307, and
Ctrl+S as `JT_KEY_SAVE` 308, both to a blocking app's `gui_poll_event` and to
a ring-3 window through the compositor's event push.

## http_post (1.9.26)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 392 | `http_post` | `struct jt_http_post *` | 0 | 0 | reply body bytes, -status, or -errno |

```c
struct jt_http_post {
    const char *path;                 /* same rules as http_get */
    const char *body; unsigned int body_len;   /* at most 6144 bytes */
    char *out;        unsigned int out_len;    /* clamped to 8192 */
    unsigned int reply_ticks;         /* 0 = 1500 (15s), clamped to 4500 (45s) */
};
```

**http_post** is the first call of the Samantha port (docs/ARCHITECTURE.md,
"Samantha at ring 3"): one `POST <path> HTTP/1.0` of `application/json` to the
chat host the Settings app keeps (`llm_host:llm_port`, turing.heyitsmejosh.com
port 80 by default). Six arguments do not fit three registers, so `ebx` names
one struct, copied out of user memory once after `paging_user_range_ok` on the
whole of it; the user copy is never read again. `path` gets the exact checks
`http_get` gives it: at most 128 bytes before the NUL, starts with `/`,
printable ASCII only. `body` is range checked for `body_len` bytes and refused
with -EINVAL above 6144, then copied into a kernel bounce buffer before the
network is touched, so a program cannot rewrite the request mid-send. `out` is
range checked for `out_len` (clamped to 8192) before anything runs. The wait
runs with interrupts on, like `http_get`, and shares its one-fetch-at-a-time
flag: a second caller gets -EBUSY.

On HTTP 200 the reply body is copied out, at most `out_len` bytes, and the
count is returned. Any other status is minus that status, -100 to -599.
-ENODEV no NIC, -EIO no answer within `reply_ticks`, -EFAULT any range outside
user memory. Nothing is written to `out` on any failure.

## audio (1.9.26)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 393 | `audio` | op: 1 play, 2 status, 3 stop | `struct jt_audio_play *` / `struct jt_audio_status *` / 0 | `sizeof` that struct | play: bytes taken; status: bytes written; stop: 0; or -errno |

One number, three ops; recording has its own number, 394, below. The format is the one the kernel's own Samantha already feeds the card: 8-bit unsigned mono PCM, 4000 to 44100 Hz (the Worker's `/api/speak` sends 16000). The program fetches the clip itself with `http_post` and queues it here.

```c
struct jt_audio_play { const void *pcm; unsigned len; unsigned rate; unsigned flags; }; /* flags: JT_AUDIO_END = 1 */
struct jt_audio_status { unsigned version, size, playing, queued, space, rate, played; };
```

`play` copies at most 8192 bytes per call into a 32KB kernel ring and returns how many it took. 0 means the ring is full (or the card is busy with the kernel's own chat playback or a recording), so retry on a later frame; the call never waits. The SB16 IRQ drains the ring in 4KB DMA transfers, so playback runs with no caller. It starts when 4KB are queued, or at once when the call carries `JT_AUDIO_END`, which the program sets on the call with the clip's last bytes (a call cut short by a full ring drops the flag, so resend it with the rest). `rate` is read only when the queue was idle. -ENODEV no card, -EINVAL zero length, bad op or short struct, -EFAULT a range outside user memory.

`status` fills the struct (`version` first, `size` is what the kernel wrote) and returns the byte count. `played` counts samples heard since the queue last went idle, interpolated inside the transfer in flight, so mouth time in ms is `played * 1000 / rate`. `stop` drops what is not yet in flight; the current 4KB, about a quarter second at 16 kHz, still finishes. The kernel's `sb16_play` and `sb16_record` return 0 while the queue is busy, and `play` returns 0 while they own the card.

### audio_record (394)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 394 | `audio_record` | op: 1 start, 2 read, 3 stop | start: rate in Hz (a value) / read: user buffer / 0 | 0 / max bytes / 0 | start: 0; read: bytes copied; stop: 0; or -errno |

Push-to-talk capture on the same card and in the same format as `audio` (8-bit unsigned mono, 4000 to 44100 Hz, 16000 for Samantha). `start` clears a 32KB kernel ring and arms capture; the SB16 IRQ then chains 4KB ADC transfers into it (about a quarter second each at 16 kHz), so nothing waits on a caller. `read` copies what has been captured so far, oldest first, at most 8192 bytes per call, and returns the count, 0 when nothing is banked yet. A caller more than 32KB behind loses the oldest samples. `stop` ends capture; the transfer in flight still lands and stays readable, so a program drains with `read` until it returns 0 after `stop`. A `start` on a take left on restarts it.

Capture and playback are exclusive in both directions. `start` returns -EBUSY while `audio` has anything queued or in flight (and while the previous take's last transfer is still landing, so retry on a later frame); `audio` `play` returns 0 while capture runs, and `sb16_play`/`sb16_record` return 0 too. -ENODEV no card, -EINVAL zero length or bad op, -EFAULT a range outside user memory. Same driver and same DSP-2.xx ADC command as the kernel chat's `sb16_record`; QEMU's `-device sb16` does not implement ADC, so under QEMU `start` succeeds but no samples arrive (as with the kernel's own F2). There is no AC97 driver in the kernel.

## sysinfo and launch_request (1.9.26)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 395 | `sysinfo` | `struct jt_sysinfo *` | caller's `sizeof` | 0 | bytes written, or -errno |
| 396 | `launch_request` | `const char *name` | 0 | 0 | 0, or -errno |

```c
struct jt_sysinfo {
    unsigned int version;      /* 1; always first so the struct can grow */
    unsigned int size;         /* sizeof in the kernel that filled it */
    unsigned int phone;        /* 1 in phone mode */
    unsigned int epoch;        /* seconds since 1970, same clock as time */
    unsigned int wx_have;      /* 1 when a good weather reading exists */
    unsigned int wx_state;     /* 0 none, 1 ok, 2 offline, 3 timeout, 4 failed, 5 bad */
    int wx_temp_c, wx_code10;  /* whole degrees C, WMO code times 10 */
    unsigned int llm_port;
    char wx_text[24];          /* the menu bar text, empty when none */
    char llm_host[40];         /* the chat host http_post talks to */
};
```

**sysinfo** hands a program the read-only state Samantha reports. `ecx` is the
size the caller was built with; `paging_user_range_ok` checks that many bytes,
the kernel fills a private copy and copies out the smaller of the two sizes, so
an old program on a new kernel and a new one on an old kernel both work (check
`size`). `ecx` under 8 is -EINVAL, a range outside user memory is -EFAULT.
Nothing a program passes in is read.

**launch_request** is `open <app>`. The name is copied out with a 24 byte
bound and must match an `APPS[]` row exactly (case sensitive, real apps only,
not the Apps folder or Trash): -EINVAL for no match, empty or too long, -EFAULT
for a bad pointer. The kernel stores one pending index and returns 0, or -EBUSY
if one is already waiting. It never launches inside the gate: the desktop loop
takes the index on its next pass and opens it the way a dock click does, as a
window for a ring-3 app or through the blocking path otherwise. Numbers 393
and 394 are the audio calls.
