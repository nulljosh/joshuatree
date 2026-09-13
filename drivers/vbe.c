/* Bochs VBE extension registers, the interface QEMU's std VGA device
   actually implements (there is no real VESA BIOS reachable from 32-bit
   protected mode without a real-mode monitor, which this deliberately
   avoids building). */
#include "vbe.h"
#include "pci.h"

typedef unsigned short u16;

static inline void outw(u16 port, u16 val) { __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port)); }

#define VBE_INDEX_PORT 0x1CE
#define VBE_DATA_PORT  0x1CF

#define VBE_INDEX_XRES   1
#define VBE_INDEX_YRES   2
#define VBE_INDEX_BPP    3
#define VBE_INDEX_ENABLE 4

#define VBE_ENABLED 0x01
#define VBE_LFB     0x40

static void vbe_write(u16 index, u16 value) {
    outw(VBE_INDEX_PORT, index);
    outw(VBE_DATA_PORT, value);
}

int vbe_set_mode(unsigned int width, unsigned int height, unsigned int bpp, unsigned int *fb_addr) {
    struct pci_device dev;
    if (!pci_find_device(0x03, 0x00, &dev)) return 0;

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
}
