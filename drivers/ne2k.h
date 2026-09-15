#ifndef NE2K_H
#define NE2K_H
/* NE2000 (8390-family) NIC driver, polled, same shape as rtl8139.c. Exists
   specifically because v86 (the JS/wasm x86 emulator the landing page's
   browser demo boots) doesn't emulate RTL8139 hardware at all (confirmed
   by grep: zero hits for it anywhere in landing/v86/libv86.js), so the
   demo's wallpaper feature (wall_fetch, a real network fetch) has never
   been reachable in the browser, only under real QEMU. v86 DOES emulate an
   NE2000-compatible card (its own source calls it "ne2k", PCI vendor
   0x10EC device 0x8029, an RTL8029 clone, confirmed against
   landing/v86/libv86.js's own pci_space table rather than assumed from
   general datasheet knowledge). This driver targets that chip. See
   roadmap.md's "fetch-mode network relay" entry for the full investigation
   that led here. */

/* Resets and configures the card (RX ring, TX buffer, PROM MAC read).
   Returns 1 on success, 0 if no RTL8029/NE2000-compatible device was found
   via PCI (vendor 0x10EC, device 0x8029) or reset failed. */
int ne2k_init(void);

/* Copies the card's burned-in MAC address (6 bytes, read from the NIC's
   own PROM via remote DMA, not assumed pre-loaded into PAR) into mac. */
void ne2k_get_mac(unsigned char mac[6]);

/* Sends one raw Ethernet frame via remote-DMA write into the card's TX
   buffer page, then triggers transmit and waits for PTX (packet
   transmitted) in the ISR. Returns 1 on success, 0 on timeout. len must be
   <= 1536 (6 pages, this driver's TX buffer size). */
int ne2k_send(const void *data, unsigned int len);

/* Non-blocking: copies the next received frame (if any) into buf (up to
   maxlen bytes, CRC already excluded by the NIC) out of the RX ring and
   returns its length, or 0 if nothing has arrived. Same clamping
   discipline as rtl8139_receive: the NIC's own reported length is never
   trusted past maxlen, see ne2k_clamp_len in ne2k.c. */
unsigned int ne2k_receive(void *buf, unsigned int maxlen);

/* Selftest for the RX length clamp, no hardware needed, mirrors
   rtl8139_clamp_selftest exactly (see ne2k.c). */
int ne2k_clamp_selftest(void);
#endif
