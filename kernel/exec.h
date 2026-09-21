#ifndef EXEC_H
#define EXEC_H
/* Loads a flat (headerless) binary off the active VFS backend and runs it
   as a real ring-3 task.
 *
 * This used to be exec_flat(), which loaded the same kind of file into a
 * kmalloc'd buffer and called it as a cdecl void(void) function -- at ring
 * 0, on the shell's own kernel stack, with the kernel's own page
 * directory. That was honest for v3 (there was no user mode yet and the
 * header said so), and wrong to keep once there was: a "user program" with
 * full kernel privileges is not a user program, and it means the syscall
 * ABI is never actually exercised, because such a binary can just call
 * kernel functions directly. Every ABI guarantee -- pointer validation,
 * -EFAULT instead of a kernel read, IOPL 0 -- only exists on the ring-3
 * path, so the reference program has to take it.
 *
 * The image is loaded at JT_USER_BASE, which is a fixed address, not a
 * relocation: a flat binary has no headers and no relocation entries, so
 * it only works where it was linked (user/hello.ld links there). The
 * window is reserved by boot/linker.ld so the physical memory manager
 * never hands those frames to the heap, and exec_user() marks its pages
 * user-accessible before the task starts.
 *
 * One program at a time, like ring3.c's payload pages: there is one image
 * window and one user stack page, so a second exec_user() while one is
 * running would load over the first. The shell waits for the task it
 * started, which is what keeps that true today.
 *
 * v2 adds argv. A flat binary has no crt0 to unpack a Linux-shaped
 * initial stack, so the loader builds the stack a plain C function would
 * expect to find, and that is the whole mechanism: at the moment the CPU
 * lands on the entry point, esp points at a fake return address, with
 * argc at 4(%esp) and argv at 8(%esp). That is the i386 cdecl frame, so
 * `void _start(int argc, char **argv)` compiles to code that reads the
 * right two words with no assembly shim anywhere. The fake return address
 * is 0 and is not a mistake: _start has no caller, and returning from it
 * jumps to an unmapped address, which idt.c's ring-3 fault path reaps
 * like any other faulting program rather than wandering into whatever
 * happened to be on the stack.
 *
 * The strings and the pointer array live at the top of the program's own
 * stack page, which exec_user() has already zeroed and made user
 * accessible. argv[argc] is NULL.
 */

#define JT_USER_BASE       0xC0500000u /* must match user/hello.ld and boot/linker.ld's .userimg */
#define JT_USER_IMAGE_MAX  (7 * 4096)  /* 28KB of code+data; page 8 of the window is the stack */
#define JT_USER_STACK_TOP  (JT_USER_BASE + 8 * 4096)

/* Bounds on the argument block, small on purpose: it is carved out of the
   top of the one 4KB stack page the program also runs on, so every byte
   spent here is a byte of stack the program does not get. A request past
   either limit fails the exec outright rather than handing the program a
   quietly truncated argv it has no way to detect. */
#define JT_ARGC_MAX   8    /* including argv[0] */
#define JT_ARGV_BYTES 256  /* total bytes of argument text, NULs included */

/* Runs `name` at ring 3 with `argc`/`argv` on its stack and waits for it
   to end. Returns 1 if the program ran (its exit status, as recorded by
   task_exit_with, goes in *status), 0 if the file could not be read, the
   argument block did not fit, or no task slot was free. A program that
   faults instead of exiting is still reaped by idt.c's ring-3 path and
   still returns 1, with whatever status that path recorded.

   argv[0] is the program's own name by convention, and it is the caller's
   job to put it there; nothing here invents one. argc may be 0, in which
   case the program gets argc == 0 and an argv holding only its NULL
   terminator. */
int exec_user(const char *name, const char *const *argv, int argc, int *status);

/* 1.0.0: a real shell launches a program by name, not just exec's exact
 * on-disk spelling. Resolves `typed` to a filename that actually exists
 * on the active VFS backend, trying in order: `typed` as-is, an
 * upper-cased copy, and the upper-cased copy with JT_RESOLVE_EXT appended
 * if `typed` has no '.' of its own. That's the real shape flat binaries
 * land on disk in (see tools/gen/gen_user_bin.py and how usertest/notetest
 * seed HELLO.BIN/NOTE.BIN): FAT already folds case on lookup (to_fat_name
 * in drivers/fat.c), but ramfs -- the disk-less backend every headless
 * check boots under -- does not, so the shell does the folding here
 * instead of teaching ramfs about case for one caller.
 *
 * `out` must be at least JT_RESOLVE_NAME_MAX bytes. On a match, writes
 * the resolved name into `out` and returns 1; on no match, returns 0 and
 * `out` is unspecified. Existence is probed with a 1-byte read, not a
 * real exec, so both `exec` and the shell's bare-name fallthrough can
 * call this before running anything, and the actual run still goes
 * through exec_user() -- there is only the one exec path. */
#define JT_RESOLVE_NAME_MAX 32
#define JT_RESOLVE_EXT      ".BIN"
int exec_resolve_name(const char *typed, char *out);
#endif
