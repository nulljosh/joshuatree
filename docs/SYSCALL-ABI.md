# Joshua Tree syscall ABI, v1

This is the contract a ring-3 program written against this kernel can rely
on. It exists because of what 1.0.0 has to mean here: semver's 1.0.0 claims
a stable public interface, and until there was a real external caller there
was nothing for a MAJOR version to protect. This file is that interface.

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
  top, 0xC0508000, is the initial `esp`. `boot/linker.ld` reserves that
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

## What a MAJOR version means, from here on

Changing the meaning of a number in the table above, removing a call,
changing an argument's meaning, or tightening one of the limits in "The
limits, exactly" is a breaking change and requires a MAJOR bump. Adding a
new number, adding a flag, raising a limit, or relaxing an error case is
not.

Before this, the file described an interface nothing depended on. Now at
least one real reference program is built against it and run by the
regression suite on every change, which is what makes the promise
checkable rather than stated.
