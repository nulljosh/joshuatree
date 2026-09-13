/* Bochs VBE extension registers, the interface QEMU's std VGA device
   actually implements (there is no real VESA BIOS reachable from 32-bit
   protected mode without a real-mode monitor, which this deliberately
   avoids building). */
#include "vbe.h"
#include "pci.h"

typedef unsigned char  u8;
typedef unsigned short u16;

static inline u8   inb(u16 port)         { u8 v; __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outb(u16 port, u8 v)  { __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port)); }
static inline void outw(u16 port, u16 v) { __asm__ volatile ("outw %0,%1" :: "a"(v), "Nd"(port)); }

#define VBE_INDEX_PORT 0x1CE
#define VBE_DATA_PORT  0x1CF

#define VBE_INDEX_XRES        1
#define VBE_INDEX_YRES        2
#define VBE_INDEX_BPP         3
#define VBE_INDEX_ENABLE      4
#define VBE_INDEX_BANK        5
#define VBE_INDEX_VIRT_WIDTH  6
#define VBE_INDEX_VIRT_HEIGHT 7
#define VBE_INDEX_X_OFFSET    8
#define VBE_INDEX_Y_OFFSET    9

#define VBE_ENABLED 0x01
#define VBE_LFB     0x40

static void vbe_write(u16 index, u16 value) {
    outw(VBE_INDEX_PORT, index);
    outw(VBE_DATA_PORT, value);
}

/* Standard VGA registers the Bochs VBE extension's own "enable" path
   doesn't touch, but that a resolution/depth switch still leaves in a
   mode-3-incompatible state. Save the real values right before switching
   away, restore those exact values on disable: correct by construction,
   not a guess at a "standard mode 3" table. Write order and the
   Attribute Controller flip-flop handling checked against a real working
   kernel's own mode-set routine (MentOS's __vga_set_mode) rather than
   trusted from memory, and confirmed correct a second way: a real
   register-snapshot diff, read back over these exact ports after
   restoring, matched the pristine pre-graphics state byte-for-byte. */
#define SEQ_INDEX      0x3C4
#define SEQ_DATA       0x3C5
#define CRTC_INDEX     0x3D4
#define CRTC_DATA      0x3D5
#define GC_INDEX       0x3CE
#define GC_DATA        0x3CF
#define AC_INDEX_DATA  0x3C0
#define AC_READ        0x3C1
#define INPUT_STATUS1  0x3DA
#define MISC_WRITE     0x3C2
#define MISC_READ      0x3CC

static u8  saved_misc;
static u8  saved_seq[5];
static u8  saved_crtc[25];
static u8  saved_gc[9];
static u8  saved_ac[21];
static int saved_valid = 0;

static void save_vga_text_state(void) {
    saved_misc = inb(MISC_READ);
    for (int i = 0; i < 5; i++)  { outb(SEQ_INDEX,  (u8)i); saved_seq[i]  = inb(SEQ_DATA); }
    for (int i = 0; i < 25; i++) { outb(CRTC_INDEX, (u8)i); saved_crtc[i] = inb(CRTC_DATA); }
    for (int i = 0; i < 9; i++)  { outb(GC_INDEX,   (u8)i); saved_gc[i]   = inb(GC_DATA); }
    for (int i = 0; i < 21; i++) {
        (void)inb(INPUT_STATUS1); /* reading this resets the AC index/data flip-flop */
        outb(AC_INDEX_DATA, (u8)i);
        saved_ac[i] = inb(AC_READ);
    }
    (void)inb(INPUT_STATUS1);
    outb(AC_INDEX_DATA, 0x20); /* re-enable video output (PAS bit) after reading the palette */
    saved_valid = 1;
}

static void restore_vga_text_state(void) {
    if (!saved_valid) return;

    outb(MISC_WRITE, saved_misc);
    for (int i = 0; i < 5; i++) { outb(SEQ_INDEX, (u8)i); outb(SEQ_DATA, saved_seq[i]); }

    /* CRTC registers 0-7 are write-protected unless bit 7 of index 0x11 is
       clear; unlock first, then the loop's own pass over index 0x11
       restores whatever lock state was actually saved. */
    outb(CRTC_INDEX, 0x11);
    outb(CRTC_DATA, (u8)(inb(CRTC_DATA) & 0x7F));
    for (int i = 0; i < 25; i++) { outb(CRTC_INDEX, (u8)i); outb(CRTC_DATA, saved_crtc[i]); }

    for (int i = 0; i < 9; i++) { outb(GC_INDEX, (u8)i); outb(GC_DATA, saved_gc[i]); }

    for (int i = 0; i < 21; i++) {
        (void)inb(INPUT_STATUS1);
        outb(AC_INDEX_DATA, (u8)i);
        outb(AC_INDEX_DATA, saved_ac[i]);
    }
    (void)inb(INPUT_STATUS1);
    outb(AC_INDEX_DATA, 0x20);
}

int vbe_set_mode(unsigned int width, unsigned int height, unsigned int bpp, unsigned int *fb_addr) {
    struct pci_device dev;
    if (!pci_find_device(0x03, 0x00, &dev)) return 0;

    save_vga_text_state(); /* must happen while still in real text mode */

    vbe_write(VBE_INDEX_ENABLE, 0); /* disable before changing resolution, per spec */
    vbe_write(VBE_INDEX_XRES, (u16)width);
    vbe_write(VBE_INDEX_YRES, (u16)height);
    vbe_write(VBE_INDEX_BPP, (u16)bpp);
    vbe_write(VBE_INDEX_ENABLE, VBE_ENABLED | VBE_LFB);

    *fb_addr = dev.bar0;
    return 1;
}

void vbe_disable(void) {
    vbe_write(VBE_INDEX_ENABLE, 0);

    /* Bochs's own virtual-scanline-width/bank/offset registers, separate
       from standard VGA, reset for hygiene: a truecolor graphics mode
       sets these far wider than a text mode's stride, disabling the
       extension alone doesn't reset them. Didn't turn out to be the
       actual cause of what briefly looked like a real bug (see the
       screenshot-artifact note below), but leaving them at a stale
       graphics-mode value is still wrong on principle. */
    vbe_write(VBE_INDEX_BANK, 0);
    vbe_write(VBE_INDEX_VIRT_WIDTH, 0);
    vbe_write(VBE_INDEX_VIRT_HEIGHT, 0);
    vbe_write(VBE_INDEX_X_OFFSET, 0);
    vbe_write(VBE_INDEX_Y_OFFSET, 0);

    restore_vga_text_state();
}
