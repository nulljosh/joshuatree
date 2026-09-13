/* RTL8139 NIC driver. Register offsets and the reset/init sequence follow
   the card's own documented behavior (same one every hobby-OS RTL8139
   driver implements, there's only one correct sequence here, not several
   equally-valid choices). */
#include "rtl8139.h"
#include "pci.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

static inline u8   inb(u16 p)          { u8 v;  __asm__ volatile ("inb %1,%0"  : "=a"(v) : "Nd"(p)); return v; }
static inline void outb(u16 p, u8 v)   { __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline u32  inl(u16 p)          { u32 v; __asm__ volatile ("inl %1,%0"  : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(u16 p, u32 v)  { __asm__ volatile ("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline u16  inw(u16 p)          { u16 v; __asm__ volatile ("inw %1,%0"  : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(u16 p, u16 v)  { __asm__ volatile ("outw %0,%1" :: "a"(v), "Nd"(p)); }

/* register offsets, relative to the I/O BAR */
#define REG_MAC0    0x00
#define REG_RBSTART 0x30
#define REG_CR      0x37
#define REG_CAPR    0x38
#define REG_TSAD0   0x20
#define REG_TSD0    0x10
#define REG_RCR     0x44
#define REG_CONFIG1 0x52
#define REG_ISR     0x3E

#define ISR_ROK 0x01

static u16 io_base = 0;
static u32 rx_offset = 0;

/* ponytail: the whole first 4MB is identity-mapped (virtual == physical),
   so these static buffers can be handed to the NIC's DMA registers as-is,
   no separate physical-address translation needed the way the v6
   framebuffer required one. That stops being true the day higher-half
   lands, whoever does that has to fix this too. */
static u8 rx_buffer[8192 + 16 + 1500] __attribute__((aligned(4)));
static u8 tx_buffer[1792]             __attribute__((aligned(4)));

int rtl8139_init(void) {
    struct pci_device dev;
    if (!pci_find_device(0x02, 0x00, &dev)) return 0;
    if (dev.bar0_is_io) io_base = (u16)dev.bar0; else return 0; /* RTL8139's BAR0 is always I/O space */

    pci_enable_device(&dev);

    outb(io_base + REG_CONFIG1, 0x00); /* power on */

    outb(io_base + REG_CR, 0x10); /* software reset */
    int timeout = 1000000;
    while ((inb(io_base + REG_CR) & 0x10) && timeout--) {}
    if (timeout <= 0) return 0;

    outl(io_base + REG_RBSTART, (u32)rx_buffer);
    outb(io_base + REG_CR, 0x0C);       /* enable RX and TX */
    outl(io_base + REG_RCR, 0x0F | 0x80); /* accept all packet types, wrap the RX ring */

    return 1;
}

void rtl8139_get_mac(u8 mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = inb(io_base + REG_MAC0 + i);
}

/* RTL8139 RX ring: packets land sequentially in rx_buffer as [u16 status]
   [u16 length-including-CRC][data...], length rounded up to a 4-byte
   boundary between packets. ISR bit0 (ROK) is set on arrival regardless of
   whether interrupts are enabled, so polling it works fine without ever
   touching IMR. CAPR's "subtract 16" offset is this register's own
   documented quirk, not a bug. */
u32 rtl8139_receive(void *buf, u32 maxlen) {
    if (!(inw(io_base + REG_ISR) & ISR_ROK)) return 0;

    u16 status = *(volatile u16 *)(rx_buffer + rx_offset);
    u16 length = *(volatile u16 *)(rx_buffer + rx_offset + 2);
    if (!(status & 0x01)) return 0; /* packet-level ROK bit not set, don't trust it */

    u32 data_len = (u32)length - 4; /* strip the trailing CRC */
    u32 copy_len = data_len < maxlen ? data_len : maxlen;
    u8 *out = buf;
    for (u32 i = 0; i < copy_len; i++) out[i] = rx_buffer[rx_offset + 4 + i];

    rx_offset = (rx_offset + length + 4 + 3) & ~3u;
    rx_offset %= sizeof(rx_buffer) - (16 + 1500); /* wrap within the true ring size, not the overflow pad */
    outw(io_base + REG_CAPR, (u16)(rx_offset - 16));
    outw(io_base + REG_ISR, ISR_ROK); /* write-1-to-clear */

    return data_len;
}

int rtl8139_send(const void *data, u32 len) {
    if (len > sizeof(tx_buffer)) return 0;
    const u8 *src = data;
    for (u32 i = 0; i < len; i++) tx_buffer[i] = src[i];

    outl(io_base + REG_TSAD0, (u32)tx_buffer);
    outl(io_base + REG_TSD0, len); /* starts the transmit */

    int timeout = 1000000;
    while (!(inl(io_base + REG_TSD0) & 0x8000) && timeout--) {} /* bit15 = TOK */
    return timeout > 0;
}
