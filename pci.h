#ifndef PCI_H
#define PCI_H
/* Enumerates the PCI bus via config-space I/O ports (0xCF8/0xCFC), the same
   mechanism every x86 machine (real or QEMU) supports with no driver needed.
   This is the prerequisite for v6's graphics mode: QEMU's std VGA device
   exposes its linear framebuffer's physical address through its BAR0, which
   only PCI config space can tell us, not any fixed constant. */

/* Finds the first device matching (class, subclass). Returns 1 and fills
   bar0 with its base address register 0 (masked to a page-aligned physical
   address) if found, 0 otherwise. */
int pci_find_device(unsigned char class_code, unsigned char subclass, unsigned int *bar0);
#endif
