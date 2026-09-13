#ifndef RTL8139_H
#define RTL8139_H
/* RTL8139 NIC driver, polled (no IRQ wiring yet, ponytail: nothing needs
   asynchronous RX delivery until an actual protocol stack sits on top of
   this, add the interrupt path when that's true instead of speculatively
   now). Chosen over virtio-net for being simpler and far better documented
   for a first NIC driver. */

/* Resets and configures the card (RX/TX buffers, accept-all-packets mode).
   Returns 1 on success, 0 if no RTL8139 was found via PCI. */
int rtl8139_init(void);

/* Copies the card's burned-in MAC address (6 bytes) into mac. */
void rtl8139_get_mac(unsigned char mac[6]);

/* Sends one raw Ethernet frame. Returns 1 once the card reports the frame
   transmitted (TOK), 0 on timeout. len must be <= 1792 (one TX descriptor's
   worth) and ideally >= 60 (Ethernet's minimum frame size before the FCS). */
int rtl8139_send(const void *data, unsigned int len);

/* Non-blocking: copies the next received frame (if any) into buf (up to
   maxlen bytes, CRC already stripped) and returns its length, or 0 if
   nothing has arrived. Poll this in a loop with hlt between checks rather
   than busy-spinning. */
unsigned int rtl8139_receive(void *buf, unsigned int maxlen);
#endif
