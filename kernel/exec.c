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
static u32 build_user_stack_at(const char *const *argv, int argc, u32 bias);
static u32 build_user_stack(const char *const *argv, int argc) { return build_user_stack_at(argv, argc, 0); }
/* 1.9.23: `bias` is kernel-pointer minus user-virtual for a private
   window (exec_user_window writes the stack page through its kmalloc'd
   buffer, while every pointer stored on it stays a JT_USER_* address). */
static u32 build_user_stack_at(const char *const *argv, int argc, u32 bias) {
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
        for (u32 j = 0; j < len; j++) ((char *)(sp + bias))[j] = a[j];
        ptrs[i] = sp;
    }

    sp &= ~3u; /* the pointer array below has to be aligned to read as u32 */

    sp -= 4; *(u32 *)(sp + bias) = 0; /* argv[argc] == NULL, so a program can walk argv without argc */
    for (int i = argc - 1; i >= 0; i--) { sp -= 4; *(u32 *)(sp + bias) = ptrs[i]; }
    u32 argv_addr = sp;

    sp -= 4; *(u32 *)(sp + bias) = argv_addr;  /* 8(%esp) at entry */
    sp -= 4; *(u32 *)(sp + bias) = (u32)argc;  /* 4(%esp) at entry */
    sp -= 4; *(u32 *)(sp + bias) = 0;          /* 0(%esp): the return address _start does not have */
    return sp;
}

static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int has_dot(const char *s) { for (; *s; s++) if (*s == '.') return 1; return 0; }

static void upper_copy(const char *in, char *out, unsigned int outsize) {
    unsigned int i = 0;
    for (; in[i] && i < outsize - 1; i++) out[i] = is_lower(in[i]) ? (char)(in[i] - 32) : in[i];
    out[i] = 0;
}

/* Existence only, no real read: bufsize 1 is enough to tell "found" (>= 0,
   however many bytes actually came back) from "no such file" (-1) on
   both backends, without disturbing anything exec_user() itself owns. */
static int vfs_exists(const char *name) {
    u8 probe[1];
    return vfs_read_file(name, probe, sizeof probe) >= 0;
}

int exec_resolve_name(const char *typed, char *out) {
    if (vfs_exists(typed)) {
        unsigned int i = 0;
        for (; typed[i] && i < JT_RESOLVE_NAME_MAX - 1; i++) out[i] = typed[i];
        out[i] = 0;
        return 1;
    }

    char upper[JT_RESOLVE_NAME_MAX];
    upper_copy(typed, upper, sizeof upper);
    if (vfs_exists(upper)) { u32 i = 0; while (upper[i]) { out[i] = upper[i]; i++; } out[i] = 0; return 1; }

    if (!has_dot(upper)) {
        char withext[JT_RESOLVE_NAME_MAX];
        unsigned int i = 0;
        for (; upper[i] && i < sizeof(withext) - sizeof(JT_RESOLVE_EXT); i++) withext[i] = upper[i];
        const char *ext = JT_RESOLVE_EXT;
        for (unsigned int j = 0; ext[j]; j++) withext[i++] = ext[j];
        withext[i] = 0;
        if (vfs_exists(withext)) { for (unsigned int k = 0; k <= i; k++) out[k] = withext[k]; return 1; }
    }
    return 0;
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
    /* 1.9.26: the arena heap for Notes and Terminal, zeroed and user-accessible while this program runs. */
    for (u32 i = 0; i < JT_USER_HEAP_BYTES; i++) ((u8 *)JT_USER_HEAP)[i] = 0;
    for (u32 off = 0; off < JT_USER_HEAP_BYTES; off += 4096) paging_set_user((void *)(JT_USER_HEAP + off));

    int id = task_create_user(JT_USER_BASE, esp);
    if (id < 0) return 0;

    serial_puts("exec: started ring-3 task from VFS\n");
    while (task_used(id)) yield(); /* the shell blocks on its child, which is also what keeps "one program at a time" true */

    paging_clear_user((void *)JT_USER_HEAP, JT_USER_HEAP_BYTES);
    if (status) *status = task_last_exit_code();
    return 1;
}

/* 1.9.23: the non-blocking twin of exec_user, for compositor windows.
   The image and stack go into a kmalloc'd, page-aligned buffer of the
   window's own (never the shared JT_USER_BASE frames), mapped at the same
   JT_USER_BASE virtual address inside this task's private page table
   (paging_task_map_private), so the binary links exactly as before and
   two programs can run at once without seeing each other's memory. Returns
   the task id, or -1; the caller owns `image` until the task is gone. */
#include "kheap.h"
int exec_user_window(const char *name, const char *const *argv, int argc, void **image_out) {
    u32 span = JT_USER_STACK_TOP - JT_USER_BASE;
    u8 *raw = (u8 *)kmalloc(span + JT_USER_HEAP_BYTES + 4096);
    if (!raw) return -1;
    u8 *img = (u8 *)(((u32)raw + 4095) & ~4095u);
    for (u32 i = 0; i < span + JT_USER_HEAP_BYTES; i++) img[i] = 0;
    int n = vfs_read_file(name, img, JT_USER_IMAGE_MAX);
    if (n <= 0) { kfree(raw); return -1; }
    u32 esp = build_user_stack_at(argv, argc, (u32)img - JT_USER_BASE);
    if (!esp) { kfree(raw); return -1; }
    int id = task_create_user(JT_USER_BASE, esp);
    if (id < 0) { kfree(raw); return -1; }
    if (!paging_task_map_private(task_page_dir(id), JT_USER_BASE, (u32)img, span) ||
        !paging_task_map_private(task_page_dir(id), JT_USER_HEAP, (u32)img + span, JT_USER_HEAP_BYTES)) { task_kill(id); kfree(raw); return -1; }
    *image_out = raw;
    serial_puts("exec: started ring-3 window task from VFS\n");
    return id;
}
