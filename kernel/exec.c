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

int exec_user(const char *name, int *status) {
    if (status) *status = -1;

    /* Zero the whole window first, image pages and stack page both. Not
       hygiene for its own sake: a flat binary has no .bss (user/hello.ld
       refuses one for exactly this reason) and the previous program's
       bytes are still sitting here, so anything the new image does not
       overwrite would otherwise be the last program's code and data,
       readable by a program that has no business reading it. */
    for (u32 i = 0; i < JT_USER_STACK_TOP - JT_USER_BASE; i++) user_image[i] = 0;

    int n = vfs_read_file(name, user_image, JT_USER_IMAGE_MAX);
    if (n <= 0) return 0;

    /* User-accessible only now, and only the pages the program actually
       gets: the image window plus the one stack page. Everything else in
       the base map stays supervisor-only. */
    for (u32 off = 0; off < JT_USER_STACK_TOP - JT_USER_BASE; off += 4096)
        paging_set_user(user_image + off);

    int id = task_create_user(JT_USER_BASE, JT_USER_STACK_TOP);
    if (id < 0) return 0;

    serial_puts("exec: started ring-3 task from VFS\n");
    while (task_used(id)) yield(); /* the shell blocks on its child, which is also what keeps "one program at a time" true */

    if (status) *status = task_last_exit_code();
    return 1;
}
