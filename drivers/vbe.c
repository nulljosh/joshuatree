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
#define DAC_INDEX_WRITE 0x3C8
#define DAC_DATA        0x3C9

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

/* The framebuffer the bootloader already set up (multiboot info, flag bit 12),
   recorded by kmain before anything else touches video. Zero when there is none. */
static unsigned int boot_fb_addr, boot_fb_pitch, boot_fb_w, boot_fb_h, boot_fb_bpp;
static int using_boot_fb;
void vbe_set_boot_framebuffer(unsigned int addr, unsigned int pitch, unsigned int w, unsigned int h, unsigned int bpp) {
    boot_fb_addr = addr; boot_fb_pitch = pitch; boot_fb_w = w; boot_fb_h = h; boot_fb_bpp = bpp;
}

int vbe_set_mode(unsigned int width, unsigned int height, unsigned int bpp, unsigned int *fb_addr) {
    struct pci_device dev;
    using_boot_fb = 0;
    if (!pci_find_device_vid(0x1234, 0x1111, &dev)) {
        /* Not the Bochs adapter, so this kernel cannot switch modes itself. Use the
           bootloader's framebuffer when it is exactly the mode asked for.
           ponytail: exact match only (same size, 32bpp, no row padding); window.c
           assumes stride == width. Add a pitch-aware blit and a scaler when real
           machines turn up that cannot do 1920x1080x32. */
        if (boot_fb_addr && boot_fb_w == width && boot_fb_h == height && boot_fb_bpp == bpp
            && bpp == 32 && boot_fb_pitch == width * 4) {
            *fb_addr = boot_fb_addr; using_boot_fb = 1; return 1;
        }
        return 0;
    }
    /* v0.x (ISO/real-hardware audit): matching any class-0x03/0x00 PCI
       device used to be enough, because every machine this kernel had
       ever booted on (QEMU's default -vga std) happened to be the Bochs
       display adapter too. Booting the ISO on other backends (QEMU
       -vga cirrus/-vga vmware, or a real GPU on real hardware) breaks
       that assumption: pci_find_device still matches (any of those is
       still PCI class 0x03/0x00), but the writes below only mean
       anything to the specific Bochs/"QEMU stdvga" DISPI interface
       (I/O ports 0x1CE/0x1CF). On anything else those writes land on
       nothing, *fb_addr still gets set to that device's BAR0 (which may
       not even be a linear framebuffer, or may be a different size/
       format than requested), and the caller would go on to map and
       write into it as if the requested mode had actually been set.
       Matching the exact vendor:device (0x1234:0x1111, QEMU/Bochs-VBE,
       the same ID real Bochs and every "std" VGA QEMU machine type
       exposes) makes this fail cleanly instead: gui_run's own
       `if (!window_open_scaled(...))` check already turns that into a
       graceful "no VGA device found" instead of a crash or garbage
       framebuffer. No driver for cirrus/vmware/real GPUs is added here;
       this only makes the existing code honest about which hardware it
       actually supports. */
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
    if (using_boot_fb) return; /* nothing of ours to undo, and the VGA register pokes below mean nothing to a GOP framebuffer */
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

/* Programs standard VGA mode 3 (80x25, 16-color text) from first
   principles, the exact register values a real VGA BIOS's own INT 10h
   mode-3 call would write, not a guess. Real hardware and QEMU both
   already boot into this mode via their own BIOS before a multiboot
   kernel ever gets control, so this kernel never needed to set it itself,
   it just wrote straight to 0xB8000 and trusted the inherited state. That
   assumption breaks under v86 (a JS/wasm x86 emulator with no BIOS in its
   multiboot boot path): confirmed by direct inspection that the kernel
   was booting and writing correct bytes to 0xB8000 the whole time, CRTC's
   own screen_width/screen_height just stayed 0 because nothing had ever
   told the emulated VGA card it was in text mode at all. Calling this
   once at the very start of kmain, before any VGA output, makes the
   kernel correct on its own terms instead of quietly depending on
   whichever BIOS happened to run first. */
void vga_text_mode_init(void) {
    static const u8 seq[5]  = {0x03, 0x00, 0x03, 0x00, 0x02};
    static const u8 crtc[25] = {
        0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F,
        0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50, 0x9C, 0x0E, 0x8F, 0x28,
        0x1F, 0x96, 0xB9, 0xA3, 0xFF
    };
    static const u8 gc[9]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF};
    static const u8 ac[21]  = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A,
        0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00
    };

    outb(MISC_WRITE, 0x67);
    for (int i = 0; i < 5; i++) { outb(SEQ_INDEX, (u8)i); outb(SEQ_DATA, seq[i]); }

    outb(CRTC_INDEX, 0x11);
    outb(CRTC_DATA, (u8)(inb(CRTC_DATA) & 0x7F)); /* unlock CRTC 0-7 before the loop writes them */
    for (int i = 0; i < 25; i++) { outb(CRTC_INDEX, (u8)i); outb(CRTC_DATA, crtc[i]); }

    for (int i = 0; i < 9; i++) { outb(GC_INDEX, (u8)i); outb(GC_DATA, gc[i]); }

    for (int i = 0; i < 21; i++) {
        (void)inb(INPUT_STATUS1);
        outb(AC_INDEX_DATA, (u8)i);
        outb(AC_INDEX_DATA, ac[i]);
    }
    (void)inb(INPUT_STATUS1);
    outb(AC_INDEX_DATA, 0x20);

    /* The attribute controller above maps each 4-bit text attribute nibble
       to one of these 16 DAC color registers, but the DAC's actual RGB
       values behind them are separate hardware state again, real silicon
       has a hardwired default palette burned in at power-on, no BIOS
       execution required, and QEMU's std VGA device models that same
       power-on default. v86 has no "power-on" to model it from: found by
       direct inspection that text bytes were landing in the row divs with
       the right characters but both foreground and background colors
       reading back as rgb(0,0,0), 0x07 (light gray on black) rendering as
       black on black. These are the standard 16-color VGA default DAC
       values (6-bit per channel, 0-63), scaled to 8-bit by *4 like a real
       DAC's output stage does driving the analog signal. */
    static const u8 dac[16][3] = {
        {0x00,0x00,0x00}, {0x00,0x00,0x2A}, {0x00,0x2A,0x00}, {0x00,0x2A,0x2A},
        {0x2A,0x00,0x00}, {0x2A,0x00,0x2A}, {0x2A,0x15,0x00}, {0x2A,0x2A,0x2A},
        {0x15,0x15,0x15}, {0x15,0x15,0x3F}, {0x15,0x3F,0x15}, {0x15,0x3F,0x3F},
        {0x3F,0x15,0x15}, {0x3F,0x15,0x3F}, {0x3F,0x3F,0x15}, {0x3F,0x3F,0x3F}
    };
    outb(DAC_INDEX_WRITE, 0);
    for (int i = 0; i < 16; i++) {
        outb(DAC_DATA, dac[i][0]);
        outb(DAC_DATA, dac[i][1]);
        outb(DAC_DATA, dac[i][2]);
    }
}
