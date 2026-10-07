#ifndef NIC_H
#define NIC_H
/* The one seam between the IP stack (net.c, http.c) and a network card, for builds that bring their own card instead
   of the i386 PCI probe (RTL8139, then NE2000). The ARM64 build compiles net.c with -DNET_NIC and supplies these:
   today QEMU's virtio-net (arch/arm64/main.c), next the Pi's Wi-Fi or Ethernet. Frames are plain Ethernet, no CRC. */

int nic_init(void);                                        /* 1 when a card is up and nic_mac is valid */
void nic_mac(unsigned char mac[6]);
int nic_send(const void *frame, unsigned int len);         /* 1 once the card took the frame */
unsigned int nic_recv(void *buf, unsigned int max);        /* never blocks: a frame's length, or 0 when none waits */

/* What else net.c leans on from the kernel around it. */
unsigned int ticks(void);                                  /* 100 per second, from boot */
void serial_puts(const char *s);
#endif
