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
#define CR_BUFE 0x01 /* Command Register bit 0: RX buffer empty (v71) */

static u16 io_base = 0;
static u32 rx_offset = 0;

/* Higher-half landed: these are now normal kernel .bss statics linked at
   0xC0000000+, but the NIC does raw physical-memory DMA, it has no concept
   of the CPU's page tables at all, so its address registers need the
   physical address, not whatever &buffer happens to read as from C code
   now. A real bug this exact gap produced: MMIO register reads (MAC
   address) still worked, since those go through the CPU normally, while
   the actual transmitted frame, checked independently with a real pcap
   capture, came back all zeros, since the card was DMAing from a physical
   address that was never actually the buffer's real memory. Fixed with a
   plain KVIRT_TO_PHYS subtraction; the first 4MB stays double-mapped
   (identity low + high alias, see paging.c), so this is a fixed offset,
   not a real translation. */
#define KVIRT_TO_PHYS(addr) ((u32)(addr) - 0xC0000000)
static u8 rx_buffer[8192 + 16 + 1500] __attribute__((aligned(4)));

/* The card has 4 TX descriptors (TSAD0-3/TSD0-3, 4 bytes apart) and expects
   the driver to cycle through them in order, one per send. Reusing
   descriptor 0 for every send works exactly once and then stalls forever
   (found via a real pcap capture showing the second frame never left the
   wire, and TSD0 stuck with OWN=0/TOK=0). 4 separate buffers, one per
   descriptor, since each send is synchronous (we wait for TOK) but the
   next call may pick a different descriptor before this one's DMA read is
   fully retired. */
static u8 tx_buffer[4][1792] __attribute__((aligned(4)));
static int tx_cur = 0;

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

    /* The reset just put the chip's own TX descriptor pointer and RX ring
       write pointer back to 0; the driver's cursors have to follow. They
       did not: every caller re-runs net_init (so this) before its request,
       so from the second request of a session on, tx_cur pointed at
       descriptor 1-3 while the chip only transmits from descriptor 0, TOK
       never came, and rtl8139_send reported failure. The first weather
       fetch of a boot worked and every later one (the ten-minute refresh,
       a retry) died with "send". tools/checks/weather-app-check.sh's
       retry steps are the regression test. */
    tx_cur = 0;
    rx_offset = 0;

    outl(io_base + REG_RBSTART, KVIRT_TO_PHYS(rx_buffer));
    outb(io_base + REG_CR, 0x0C);       /* enable RX and TX */
    outl(io_base + REG_RCR, 0x0F | 0x80); /* accept all packet types, wrap the RX ring */

    return 1;
}

void rtl8139_get_mac(u8 mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = inb(io_base + REG_MAC0 + i);
}

/* RTL8139 RX ring: packets land sequentially in rx_buffer as [u16 status]
   [u16 length-including-CRC][data...], length rounded up to a 4-byte
   boundary between packets. CAPR's "subtract 16" offset is this
   register's own documented quirk, not a bug.

   v71 (0.65.0), real bug found the day weather_fetch started making TWO
   connections on one init (ip-api.com, then Open-Meteo): this used to
   gate on ISR bit0 (ROK) and write-1-clear it after reading ONE packet.
   ROK is a "something arrived" latch, not a ring-occupancy flag, so when
   two frames land between polls (a SYN-ACK and its data, or the server's
   FIN after tcp_get's best-effort close that never waits for it) the
   second stays in the ring with ROK already cleared, and only gets read
   when a THIRD frame arrives and sets ROK again: the driver runs one
   packet behind for the rest of the session after any burst. Seen live,
   headless: the second dns_resolve's reply arrived, the poll handed back
   the previous server's stale FIN instead, cleared ROK, and the real
   reply sat unread until the timeout ("dns-fail", http_get -1). Every
   earlier caller (web, weathertest, chat) happened to call rtl8139_init
   right before its one connection, which resets the ring and hid this
   for 60+ versions. The fix is the standard one: gate on the Command
   Register's BUFE bit (bit 0, "RX buffer empty"), which reflects the
   real ring state, exactly what the OSDev RTL8139 page's receive loop
   and Linux's rtl8139_rx (`while ((RTL_R8(ChipCmd) & RxBufEmpty) == 0)`)
   both do. ROK is still write-1-cleared below so the ISR doesn't stay
   latched, it just no longer decides whether there is data to read.
   tools/geo-check.sh is the permanent regression test: it needs both
   back-to-back connections to land (its `wx=` assertion) and fails on
   the old ROK gate. */
/* Pure logic, no hardware I/O, so it's real unit-testable the same way
   drivers/net.c's tcp_match_selftest tests tcp_match directly: given the
   NIC's own raw length field for a queued frame and the caller's buffer
   size, returns how many bytes rtl8139_receive will actually write into
   that buffer.

   Real bug this replaces (v0.72.x, found tracing net.c's own "n is the
   actual number of bytes rtl8139_receive put in rx" comment against what
   this function actually returned): rtl8139_receive used to return
   data_len, the NIC's raw claimed length minus 4 for the CRC, completely
   unclamped, while the copy loop right above it only ever wrote
   min(data_len, maxlen) bytes into the caller's buffer. Every net.c
   caller passes sizeof(rx) (1514) as maxlen and trusts the return value
   as "how many bytes are actually valid in my rx[1514]" -- tcp_match's
   own bound check (sizeof(eth)+ip_total>n, the fix for the earlier
   tcp_match OOB bug) depends on that being true. A frame whose NIC-
   reported length exceeds 1514 (or is < 4, underflowing data_len to
   roughly 4 billion) made rtl8139_receive claim far more valid bytes
   than it had written, defeating that exact check one layer down and
   reopening the identical class of OOB stack read the tcp_match fix
   was written to close, just moved from "ip_total lied" to "the NIC
   length field lied (or underflowed)". Root cause: return what was
   actually copied, not what the wire claimed. */
static u32 rtl8139_clamp_len(u16 length, u32 maxlen) {
    u32 data_len = length >= 4 ? (u32)length - 4 : 0; /* guard the CRC-strip underflow on a corrupt/runt length */
    return data_len < maxlen ? data_len : maxlen;
}

int rtl8139_clamp_selftest(void) {
    if (rtl8139_clamp_len(9000, 1514) != 1514) return 0; /* oversized claim must clamp to what's actually copied */
    if (rtl8139_clamp_len(64, 1514) != 60) return 0;      /* honest small frame: 64 - 4 (CRC) = 60 */
    if (rtl8139_clamp_len(2, 1514) != 0) return 0;        /* runt frame shorter than the CRC itself: no underflow */
    return 1;
}

u32 rtl8139_receive(void *buf, u32 maxlen) {
    if (inb(io_base + REG_CR) & CR_BUFE) return 0; /* v71: ring really empty, see the comment above */

    u16 status = *(volatile u16 *)(rx_buffer + rx_offset);
    u16 length = *(volatile u16 *)(rx_buffer + rx_offset + 2);
    if (!(status & 0x01)) return 0; /* packet-level ROK bit not set, don't trust it */

    u32 copy_len = rtl8139_clamp_len(length, maxlen);
    u8 *out = buf;
    for (u32 i = 0; i < copy_len; i++) out[i] = rx_buffer[rx_offset + 4 + i];

    rx_offset = (rx_offset + length + 4 + 3) & ~3u;
    rx_offset %= sizeof(rx_buffer) - (16 + 1500); /* wrap within the true ring size, not the overflow pad */
    outw(io_base + REG_CAPR, (u16)(rx_offset - 16));
    outw(io_base + REG_ISR, ISR_ROK); /* write-1-to-clear */

    return copy_len;
}

int rtl8139_send(const void *data, u32 len) {
    if (len > sizeof(tx_buffer[0])) return 0;
    const u8 *src = data;
    u8 *buf = tx_buffer[tx_cur];
    for (u32 i = 0; i < len; i++) buf[i] = src[i];

    u16 tsad = (u16)(REG_TSAD0 + tx_cur * 4);
    u16 tsd  = (u16)(REG_TSD0  + tx_cur * 4);
    tx_cur = (tx_cur + 1) % 4;

    outl(io_base + tsad, KVIRT_TO_PHYS(buf));
    outl(io_base + tsd, len); /* starts the transmit */

    int timeout = 1000000;
    while (!(inl(io_base + tsd) & 0x8000) && timeout--) {} /* bit15 = TOK */
    return timeout > 0;
}
