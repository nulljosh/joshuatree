/* Physical memory manager: a bitmap, one bit per 4K frame.
   ponytail: reads mem_upper (contiguous RAM above 1MB) from the multiboot
   info struct instead of walking the full mmap array, QEMU/GRUB always
   fill mem_upper, and a single contiguous region is all this machine has
   until something adds real hotplug/holes to worry about. Bitmap is a fixed
   MAX_FRAMES array (128MB worth); more RAM than that is just left untracked
   for now, upgrade to a dynamically-sized bitmap if that ever matters. */
#include "pmm.h"
#include "serial.h"

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
    u32 boot_device, cmdline, mods_count, mods_addr, syms[4];
    u32 mmap_length;
    u32 mmap_addr;
} __attribute__((packed));

/* v86 (the landing page's in-browser PC) never fills mem_upper, only the
   memory map, so without this the demo guessed 15MB and ran out of heap:
   no wallpaper, and Samantha's voice never had room to play. Returns KB of
   RAM in the map entry that starts at or covers 1MB, like mem_upper. */
static u32 mmap_upper_kb(const struct multiboot_info *mb) {
    u32 p = mb->mmap_addr, end = mb->mmap_addr + mb->mmap_length;
    while (p < end) {
        const u32 *e = (const u32 *)p;           /* size, base_lo, base_hi, len_lo, len_hi, type */
        if (e[5] == 1 && e[2] == 0 && e[4] == 0 && e[1] <= 0x100000 && e[1] + e[3] > 0x100000)
            return (e[1] + e[3] - 0x100000) / 1024;
        p += e[0] + 4;
    }
    return 0;
}

void pmm_init(u32 multiboot_info_addr) {
    struct multiboot_info *mb = (struct multiboot_info *)multiboot_info_addr;

    u32 mem_upper_kb = 0;
    if (mb && (mb->flags & 0x1)) mem_upper_kb = mb->mem_upper;
    else if (mb && (mb->flags & 0x40)) mem_upper_kb = mmap_upper_kb(mb);
    /* mem_upper is KB of RAM starting at 1MB. Fall back to a conservative
       16MB guess if the bootloader didn't report it (shouldn't happen under
       QEMU/GRUB, but better than dividing by zero). */
    u32 usable_bytes = mem_upper_kb ? mem_upper_kb * 1024 : (16 * 1024 * 1024 - 0x100000);

    { /* one boot line so a wrong RAM size is visible, not a silent 16MB guess */
        char d[12]; int n = 0; u32 v = usable_bytes / (1024 * 1024);
        do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        serial_puts(mem_upper_kb ? "pmm: ram_mb=" : "pmm: ram_mb=guess ");
        while (n) { char c[2] = { d[--n], 0 }; serial_puts(c); }
        serial_puts("\n");
    }
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
