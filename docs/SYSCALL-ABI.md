# Joshua Tree syscall ABI

This is the contract a ring-3 program written against this kernel can rely
on. It exists because of what 1.0.0 has to mean here: semver's 1.0.0 claims
a stable public interface, and until there was a real external caller there
was nothing for a MAJOR version to protect. This file is that interface.

Two versions are shipped. **v1 is frozen**: every number, argument meaning
and error below in the v1 section works exactly as written and will not
change without a MAJOR bump. **v2 adds** file writing, a real seek, and
command-line arguments. v2 adds numbers, flag bits and one loader
convention; it redefines nothing v1 said. `user/hello.c` is unchanged and
`tools/checks/usertest-check.sh` still runs it unmodified.

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
- It is linked at **0xC0500000** and gets **8 pages**: seven for the
  image (28KB, and a larger one fails to link), one for its stack, whose
  top, 0xC0508000, is the initial `esp`. (v2 pushes the argument block
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
top     0xC0508000, the top of the program's stack page
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
0xC0508000, is the initial `esp`. With arguments on the stack that is no
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
  0xC0508000 would notice, and no such program exists or could have been
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
one program running at a time, the same 7-page image ceiling. libjt does
not get around any of that, it just saves you from re-writing `strlen`
and a decimal formatter in every program that needs one.
