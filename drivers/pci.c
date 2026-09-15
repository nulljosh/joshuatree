/* PCI config-space enumeration via the legacy 0xCF8/0xCFC I/O ports. Every
   x86 machine, real or emulated, supports this even without a real driver
   for the device itself. Used by v6's graphics code (finds the framebuffer's
   physical address, a memory BAR) and v7's networking (finds the NIC's I/O
   BAR, and enables the device since nothing else does that for us). */
#include "pci.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

static inline void outl(u16 port, u32 val) { __asm__ volatile ("outl %0, %1" :: "a"(val), "Nd"(port)); }
static inline u32   inl(u16 port)          { u32 v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static u32 pci_addr(u8 bus, u8 slot, u8 func, u8 offset) {
    return 0x80000000
        | ((u32)bus << 16)
        | ((u32)slot << 11)
        | ((u32)func << 8)
        | (offset & 0xFC);
}

static u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset) {
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

static void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value) {
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

static void pci_fill_bar0(struct pci_device *dev, u8 bus, u8 slot, u8 func, u32 bar) {
    dev->bus = bus; dev->slot = slot; dev->func = func;
    if (bar & 0x1) { /* bit0 set = I/O space BAR */
        dev->bar0_is_io = 1;
        dev->bar0 = bar & 0xFFFFFFFC;
    } else {
        dev->bar0_is_io = 0;
        dev->bar0 = bar & 0xFFFFFFF0;
    }
}

/* ponytail: scans every bus/slot/func exhaustively (256*32*8 reads worst
   case) instead of walking capability lists or skipping absent buses.
   QEMU's device tree is tiny, this finishes in microseconds either way. */
int pci_find_device(u8 class_code, u8 subclass, struct pci_device *dev) {
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
                    pci_fill_bar0(dev, (u8)bus, (u8)slot, (u8)func, bar);
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Same exhaustive scan as pci_find_device, matching the exact
   (vendor_id, device_id) pair instead of (class, subclass): needed for
   drivers/ne2k.c, since RTL8139 and the RTL8029 (NE2000-compatible) clone
   QEMU/v86 emulate share the same class/subclass and pci_find_device alone
   can't tell them apart. */
int pci_find_device_vid(u16 vendor_id, u16 device_id, struct pci_device *dev) {
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 slot = 0; slot < 32; slot++) {
            for (u32 func = 0; func < 8; func++) {
                u32 id = pci_config_read32(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue; /* no device here */

                u16 found_vendor = (u16)(id & 0xFFFF);
                u16 found_device = (u16)((id >> 16) & 0xFFFF);
                if (found_vendor == vendor_id && found_device == device_id) {
                    u32 bar = pci_config_read32(bus, slot, func, 0x10);
                    pci_fill_bar0(dev, (u8)bus, (u8)slot, (u8)func, bar);
                    return 1;
                }
            }
        }
    }
    return 0;
}

void pci_enable_device(const struct pci_device *dev) {
    u32 command = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    command |= 0x01; /* I/O space enable */
    command |= 0x02; /* memory space enable */
    command |= 0x04; /* bus master enable, needed for the NIC to DMA at all */
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, command);
}
