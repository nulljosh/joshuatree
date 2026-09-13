/* Physical memory manager: a bitmap, one bit per 4K frame.
   ponytail: reads mem_upper (contiguous RAM above 1MB) from the multiboot
   info struct instead of walking the full mmap array — QEMU/GRUB always
   fill mem_upper, and a single contiguous region is all this machine has
   until something adds real hotplug/holes to worry about. Bitmap is a fixed
   MAX_FRAMES array (128MB worth); more RAM than that is just left untracked
   for now, upgrade to a dynamically-sized bitmap if that ever matters. */
#include "pmm.h"

typedef unsigned int u32;

#define FRAME_SIZE 4096
#define MAX_FRAMES (128 * 1024 * 1024 / FRAME_SIZE) /* 128MB / 4K = 32768 */

static u32 bitmap[MAX_FRAMES / 32];
static u32 total_frames = 0;
static u32 free_count = 0;

extern char _kernel_end; /* from linker.ld, first byte past the kernel image */

static void set_bit(u32 f)   { bitmap[f / 32] |= (1u << (f % 32)); }
static void clear_bit(u32 f) { bitmap[f / 32] &= ~(1u << (f % 32)); }
static int  test_bit(u32 f)  { return (bitmap[f / 32] >> (f % 32)) & 1; }

struct multiboot_info {
    u32 flags;
    u32 mem_lower;
    u32 mem_upper;
} __attribute__((packed));

void pmm_init(u32 multiboot_info_addr) {
    struct multiboot_info *mb = (struct multiboot_info *)multiboot_info_addr;

    u32 mem_upper_kb = 0;
    if (mb && (mb->flags & 0x1)) mem_upper_kb = mb->mem_upper;
    /* mem_upper is KB of RAM starting at 1MB. Fall back to a conservative
       16MB guess if the bootloader didn't report it (shouldn't happen under
       QEMU/GRUB, but better than dividing by zero). */
    u32 usable_bytes = mem_upper_kb ? mem_upper_kb * 1024 : (16 * 1024 * 1024 - 0x100000);

    total_frames = usable_bytes / FRAME_SIZE;
    if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;

    for (u32 i = 0; i < total_frames; i++) clear_bit(i);
    free_count = total_frames;

    /* reserve every frame the kernel image itself occupies (1MB .. _kernel_end) */
    u32 kernel_end_frame = ((u32)&_kernel_end - 0x100000) / FRAME_SIZE + 1;
    for (u32 i = 0; i < kernel_end_frame && i < total_frames; i++) {
        if (!test_bit(i)) { set_bit(i); free_count--; }
    }
}

u32 pmm_alloc_frame(void) {
    for (u32 i = 0; i < total_frames; i++) {
        if (!test_bit(i)) {
            set_bit(i);
            free_count--;
            return 0x100000 + i * FRAME_SIZE;
        }
    }
    return 0; /* out of memory */
}

void pmm_free_frame(u32 addr) {
    if (addr < 0x100000) return;
    u32 f = (addr - 0x100000) / FRAME_SIZE;
    if (f < total_frames && test_bit(f)) { clear_bit(f); free_count++; }
}

u32 pmm_total_frames(void) { return total_frames; }
u32 pmm_free_frames(void)  { return free_count; }
