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
 */

#define JT_USER_BASE       0xC0500000u /* must match user/hello.ld and boot/linker.ld's .userimg */
#define JT_USER_IMAGE_MAX  (7 * 4096)  /* 28KB of code+data; page 8 of the window is the stack */
#define JT_USER_STACK_TOP  (JT_USER_BASE + 8 * 4096)

/* Runs `name` at ring 3 and waits for it to end. Returns 1 if the program
   ran (its exit status, as recorded by task_exit_with, goes in *status),
   0 if the file could not be read or no task slot was free. A program
   that faults instead of exiting is still reaped by idt.c's ring-3 path
   and still returns 1, with whatever status that path recorded. */
int exec_user(const char *name, int *status);
#endif
