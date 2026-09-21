#ifndef VBE_H
#define VBE_H
/* QEMU's std VGA device speaks the Bochs VBE extension register interface
   (ports 0x1CE index / 0x1CF data), not real VESA BIOS calls, which aren't
   reachable from 32-bit protected mode without a real-mode monitor anyway.
   This switches graphics on and off at runtime, so the text-mode shell
   keeps working right up until something actually asks for a graphics
   mode, unlike requesting a video mode in the multiboot header. */

/* Enables a linear framebuffer mode at the given resolution/depth. Returns
   1 on success, 0 if no VGA device was found via PCI. fb_addr is set to
   the framebuffer's physical base address (from PCI BAR0) on success. */
int vbe_set_mode(unsigned int width, unsigned int height, unsigned int bpp, unsigned int *fb_addr);

/* Switches back to VGA text mode (0x03), restoring the shell. */
void vbe_disable(void);
void vbe_set_boot_framebuffer(unsigned int addr, unsigned int pitch, unsigned int w, unsigned int h, unsigned int bpp);

/* Programs standard VGA mode 3 (80x25 text) from scratch: the exact
   registers a real VGA BIOS's mode-3 call would set. Real hardware/QEMU
   already boot into this via their own BIOS, so this was never needed
   there; a BIOS-less multiboot environment (v86) never sets it at all.
   Call once, first thing in kmain, before any VGA output. */
void vga_text_mode_init(void);
#endif
