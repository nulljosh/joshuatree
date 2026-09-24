#ifndef E1000_H
#define E1000_H
/* Intel 82540EM (e1000) NIC driver, polled, same shape and same
   send/receive/clamp contract as rtl8139.c and ne2k.c so drivers/net.c can
   treat it as a third interchangeable NIC. Targets QEMU's `-device e1000`
   model, which emulates the real 82540EM (PCI vendor 0x8086, device
   0x100E) closely enough that this driver should also work on the real
   chip and its many workalikes. Most PCs from the last 15 years carry an
   Intel or Realtek NIC, so this and rtl8139.c together are what make real
   hardware (not just an emulator) plausible for this kernel's networking. */

/* Resets and configures the card (MMIO BAR mapped via paging_map_region,
   RX/TX descriptor rings, MAC read from the EEPROM). Returns 1 on success,
   0 if no 82540EM was found via PCI or the MMIO region couldn't be mapped. */
int e1000_init(void);

/* Copies the card's burned-in MAC address (6 bytes, read out of the
   EEPROM, not assumed pre-loaded into RAL/RAH) into mac. */
void e1000_get_mac(unsigned char mac[6]);

/* Sends one raw Ethernet frame via the TX descriptor ring. Returns 1 once
   the descriptor's own status byte reports DD (descriptor done), 0 on
   timeout. len must be <= 1518. */
int e1000_send(const void *data, unsigned int len);

/* Non-blocking: copies the next received frame (if any) off the RX ring
   into buf (up to maxlen bytes, CRC already stripped by the card) and
   returns its length, or 0 if nothing has arrived. */
unsigned int e1000_receive(void *buf, unsigned int maxlen);

/* Selftest for the RX length clamp, no hardware needed, mirrors
   rtl8139_clamp_selftest/ne2k_clamp_selftest exactly (see e1000.c). */
int e1000_clamp_selftest(void);
#endif
