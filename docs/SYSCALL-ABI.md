# Joshua Tree syscall ABI, v1

This is the contract a ring-3 program written against this kernel can rely
on. It exists because of what 1.0.0 has to mean here: semver's 1.0.0 claims
a stable public interface, and until there was a real external caller there
was nothing for a MAJOR version to protect. This file is that interface.

Everything below is either already shipped (v64 / 0.61.0) or specified here
as the v1 set. Nothing in this file is aspirational: if a call is listed as
specified but not implemented, it says so.

## Calling convention

Borrowed from x86 Linux rather than invented, so anything written against
that convention lines up here without translation:

- `int $0x80` is the gate. IDT vector 0x80, DPL 3, 32-bit interrupt gate,
  so interrupts are off for the duration of the call.
- `eax` holds the syscall number on entry and the result on return.
- `ebx`, `ecx`, `edx` hold arguments 1 through 3. There is no fourth
  argument in v1, and adding one later does not break this contract.
- A negative return value is `-errno`. Errno values are Linux's own.
- Every unassigned number returns `-ENOSYS` (38). Dispatch never jumps
  into an empty table slot.
- The call runs on the calling task's own kernel stack, repointed by the
  TSS `esp0` that `task.c`'s scheduler updates on every switch.

Any pointer a program hands the kernel is checked against the page tables
before the kernel reads or writes it, the job Linux's `access_ok` does. An
unmapped or non-user-accessible range gets `-EFAULT` (14), never a kernel
read of a kernel address on a user program's behalf.

## The v1 call set

Numbers are Linux i386's own, so `__NR_exit` is 1 and `__NR_write` is 4.
The gaps are deliberate, not slots waiting to be filled in order.

| # | Call | Arguments | Returns | Status |
|---|------|-----------|---------|--------|
| 1 | `exit` | ebx = exit code | never returns | shipped |
| 3 | `read` | ebx = fd, ecx = buf, edx = len | bytes read, 0 at end | specified |
| 4 | `write` | ebx = fd, ecx = buf, edx = len | bytes written | shipped |
| 5 | `open` | ebx = path, ecx = flags | fd, or -errno | specified |
| 6 | `close` | ebx = fd | 0, or -errno | specified |
| 13 | `time` | ebx = 0 | seconds since the epoch | specified |
| 20 | `getpid` | none | this task's id | specified |
| 158 | `sched_yield` | none | 0 | specified |

File descriptors 0, 1 and 2 are standard input, output and error. There is
one console, so 1 and 2 both reach it. `read` on fd 0 reads the keyboard.
Descriptors from `open` start at 3 and are per task.

`write` copies at most 255 bytes per call, so the kernel-side copy has a
fixed on-stack buffer. Longer output is more calls. That limit is part of
the contract, not an implementation detail to quietly change.

## What this contract does NOT cover

Honest scope, so nothing here overclaims:

- No memory-mapping call, no `brk`, no `mmap`. A v1 program gets the pages
  it was loaded with.
- No `fork` or `exec`. Tasks are created by the kernel, not by user code.
- No signals, no IPC, no sockets. The network stack is kernel-side only.
- No directory enumeration. `open`/`read`/`close` on a known path only.
- Nothing about the GUI. Every app you can click is kernel code today, not
  a ring-3 program, and converting them is a separate, larger job.

## What a MAJOR version means, from here on

Changing the meaning of a number in the table above, removing a call, or
changing an argument's meaning is a breaking change and requires a MAJOR
bump. Adding a new number, adding a flag, or relaxing an error case is not.

Before 1.0.0 this file described an interface nothing depended on. After
1.0.0, at least one real reference program is built against it and run by
the regression suite on every change, which is what makes the promise
checkable rather than stated.
