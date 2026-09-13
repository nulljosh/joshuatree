/* PCI config-space enumeration via the legacy 0xCF8/0xCFC I/O ports. Every
   x86 machine, real or emulated, supports this even without a real driver
   for the device itself. This exists specifically so v6's graphics code can
   find QEMU's std VGA device and read its linear framebuffer address out of
   BAR0, since that address is not a fixed constant, only PCI config space
   knows it. */
#include "pci.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

static inline void outl(u16 port, u32 val) { __asm__ volatile ("outl %0, %1" :: "a"(val), "Nd"(port)); }
static inline u32   inl(u16 port)          { u32 v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 address = 0x80000000
        | ((u32)bus << 16)
        | ((u32)slot << 11)
        | ((u32)func << 8)
        | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

/* ponytail: scans every bus/slot/func exhaustively (256*32*8 reads worst
   case) instead of walking capability lists or skipping absent buses.
   QEMU's device tree is tiny, this finishes in microseconds either way. */
int pci_find_device(u8 class_code, u8 subclass, u32 *bar0) {
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 slot = 0; slot < 32; slot++) {
            for (u32 func = 0; func < 8; func++) {
                u32 id = pci_config_read32(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue; /* no device here */

                u32 class_reg = pci_config_read32(bus, slot, func, 0x08);
                u8 found_class = (class_reg >> 24) & 0xFF;
                u8 found_subclass = (class_reg >> 16) & 0xFF;

                if (found_class == class_code && found_subclass == subclass) {
                    u32 bar = pci_config_read32(bus, slot, func, 0x10);
                    *bar0 = bar & 0xFFFFFFF0; /* mask off the low flag bits, keep the base address */
                    return 1;
                }
            }
        }
    }
    return 0;
}
