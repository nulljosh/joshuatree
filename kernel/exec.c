#include "exec.h"
#include "vfs.h"
#include "paging.h"
#include "task.h"
#include "serial.h"

typedef unsigned int u32;
typedef unsigned char u8;

/* The image window, reserved in boot/linker.ld as a NOLOAD section at
   JT_USER_BASE so _kernel_end covers it and pmm.c never hands those
   frames out. Declared here as a plain pointer rather than an array
   because nothing in the kernel's own C world owns it: it is a hole in
   the address space that exists to be written by exec_user() and read by
   the CPU at ring 3. */
static u8 *const user_image = (u8 *)JT_USER_BASE;

/* v2: builds the program's initial stack, arguments and all.
 *
 * The layout is chosen to be the one a plain C function already expects,
 * because there is no crt0 here to translate anything. Real Linux puts
 * argc at 0(%esp) with no return address above it, which is exactly why a
 * real i386 crt0 has to be written in assembly. A flat binary in this
 * kernel has no crt0 at all, so the loader picks the other layout and the
 * compiler does the rest:
 *
 *     esp+0   fake return address (0)
 *     esp+4   argc
 *     esp+8   argv          -> [ argv[0], ..., argv[argc-1], NULL ]
 *     ...     the argv pointer array
 *     ...     the argument strings
 *     top     JT_USER_STACK_TOP
 *
 * so `void _start(int argc, char **argv)` reads the right two words.
 *
 * Everything written here lands on the one stack page exec_user() has
 * already zeroed and marked user accessible, so the program can read its
 * own arguments and nothing else can. Returns the initial esp, or 0 if
 * the block does not fit inside JT_ARGC_MAX/JT_ARGV_BYTES, which fails
 * the exec rather than silently dropping arguments. */
static u32 build_user_stack(const char *const *argv, int argc) {
    if (argc < 0 || argc > JT_ARGC_MAX) return 0;

    u32 ptrs[JT_ARGC_MAX];
    u32 sp = JT_USER_STACK_TOP;
    u32 used = 0;

    /* Strings first, from the top down, in reverse so argv[0] ends up
       lowest; the order costs nothing and keeps a hex dump of the page
       readable in argument order. */
    for (int i = argc - 1; i >= 0; i--) {
        const char *a = argv[i] ? argv[i] : "";
        u32 len = 0;
        while (a[len]) len++;
        len++; /* the NUL is part of the string and part of the budget */
        used += len;
        if (used > JT_ARGV_BYTES) return 0;
        sp -= len;
        for (u32 j = 0; j < len; j++) ((char *)sp)[j] = a[j];
        ptrs[i] = sp;
    }

    sp &= ~3u; /* the pointer array below has to be aligned to read as u32 */

    sp -= 4; *(u32 *)sp = 0; /* argv[argc] == NULL, so a program can walk argv without argc */
    for (int i = argc - 1; i >= 0; i--) { sp -= 4; *(u32 *)sp = ptrs[i]; }
    u32 argv_addr = sp;

    sp -= 4; *(u32 *)sp = argv_addr;  /* 8(%esp) at entry */
    sp -= 4; *(u32 *)sp = (u32)argc;  /* 4(%esp) at entry */
    sp -= 4; *(u32 *)sp = 0;          /* 0(%esp): the return address _start does not have */
    return sp;
}

int exec_user(const char *name, const char *const *argv, int argc, int *status) {
    if (status) *status = -1;

    /* Zero the whole window first, image pages and stack page both. Not
       hygiene for its own sake: a flat binary has no .bss (user/hello.ld
       refuses one for exactly this reason) and the previous program's
       bytes are still sitting here, so anything the new image does not
       overwrite would otherwise be the last program's code and data,
       readable by a program that has no business reading it. That covers
       the argument block too, which is why one program's argv cannot be
       read out of the stack page by the next one. */
    for (u32 i = 0; i < JT_USER_STACK_TOP - JT_USER_BASE; i++) user_image[i] = 0;

    int n = vfs_read_file(name, user_image, JT_USER_IMAGE_MAX);
    if (n <= 0) return 0;

    u32 esp = build_user_stack(argv, argc);
    if (!esp) { serial_puts("exec: argument block too large, not started\n"); return 0; }

    /* User-accessible only now, and only the pages the program actually
       gets: the image window plus the one stack page. Everything else in
       the base map stays supervisor-only. */
    for (u32 off = 0; off < JT_USER_STACK_TOP - JT_USER_BASE; off += 4096)
        paging_set_user(user_image + off);

    int id = task_create_user(JT_USER_BASE, esp);
    if (id < 0) return 0;

    serial_puts("exec: started ring-3 task from VFS\n");
    while (task_used(id)) yield(); /* the shell blocks on its child, which is also what keeps "one program at a time" true */

    if (status) *status = task_last_exit_code();
    return 1;
}
