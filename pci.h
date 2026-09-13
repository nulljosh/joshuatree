#ifndef PCI_H
#define PCI_H
/* Enumerates the PCI bus via config-space I/O ports (0xCF8/0xCFC), the same
   mechanism every x86 machine (real or QEMU) supports with no driver needed.
   Used by both v6's graphics (finds the VGA device's framebuffer BAR, a
   memory BAR) and v7's networking (finds the NIC's BAR, which for RTL8139
   is an I/O-space BAR, a genuinely different kind of address entirely). */

struct pci_device {
    unsigned char bus, slot, func;
    unsigned int  bar0;
    int           bar0_is_io; /* 1 if bar0 is an I/O port base, 0 if it's a physical memory address */
};

/* Finds the first device matching (class, subclass). Returns 1 and fills
   dev on success, 0 if nothing matches. bar0 is already masked (the low
   address-decode bits stripped) for whichever BAR type it turned out to be. */
int pci_find_device(unsigned char class_code, unsigned char subclass, struct pci_device *dev);

/* Sets the I/O space, memory space, and bus-master enable bits in the
   device's PCI command register. Needed because a kernel entered directly
   via multiboot (no BIOS/SeaBIOS device-enumeration pass first) can't
   assume those bits are already on the way real firmware would leave them. */
void pci_enable_device(const struct pci_device *dev);
#endif
