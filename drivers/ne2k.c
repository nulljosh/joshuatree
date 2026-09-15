/* NE2000/RTL8029 NIC driver. Register offsets, the remote-DMA protocol and
   the page-addressed ring buffer follow the card's own well-documented
   8390-family behavior (OSDev wiki's "Ne2000" page, and the same shape
   used by every hobby-OS NE2000 driver, e.g. ToaruOS's). Unlike rtl8139.c,
   this card has no host-RAM DMA at all: the "RX ring" and "TX buffer" both
   live entirely inside the NIC's own internal memory, addressed by an
   8-bit page number (each page = 256 bytes) the driver programs into
   RSAR0/1 (remote start address) before every read/write, so there's no
   KVIRT_TO_PHYS translation to worry about here, a real structural
   difference from rtl8139.c's descriptor-ring-into-host-RAM design, not an
   oversight. */
#include "ne2k.h"
#include "pci.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

static inline u8   inb(u16 p)          { u8 v;  __asm__ volatile ("inb %1,%0"  : "=a"(v) : "Nd"(p)); return v; }
static inline void outb(u16 p, u8 v)   { __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline u16  inw(u16 p)          { u16 v; __asm__ volatile ("inw %1,%0"  : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(u16 p, u16 v)  { __asm__ volatile ("outw %0,%1" :: "a"(v), "Nd"(p)); }

/* Register offsets, relative to the I/O BAR. Page-0 and page-1 registers
   share the same offsets, disambiguated by the PS0/PS1 bits in CR (the one
   register that's the same on every page). */
#define REG_CR      0x00 /* command register, all pages */
#define REG_PSTART  0x01 /* page0 write: RX ring start page */
#define REG_PAR0    0x01 /* page1: physical (MAC) address, 6 bytes from here */
#define REG_PSTOP   0x02 /* page0 write: RX ring stop page */
#define REG_BNRY    0x03 /* page0: boundary pointer (our RX read cursor) */
#define REG_TPSR    0x04 /* page0 write: TX buffer start page */
#define REG_TBCR0   0x05 /* page0 write: TX byte count, low */
#define REG_TBCR1   0x06 /* page0 write: TX byte count, high */
#define REG_ISR     0x07 /* page0: interrupt status */
#define REG_CURR    0x07 /* page1: current page (NIC's RX write cursor) */
#define REG_RSAR0   0x08 /* page0 write: remote DMA start address, low */
#define REG_RSAR1   0x09 /* page0 write: remote DMA start address, high */
#define REG_RBCR0   0x0A /* page0 write: remote DMA byte count, low */
#define REG_RBCR1   0x0B /* page0 write: remote DMA byte count, high */
#define REG_RCR     0x0C /* page0 write: receive configuration */
#define REG_TCR     0x0D /* page0 write: transmit configuration */
#define REG_DCR     0x0E /* page0 write: data configuration */
#define REG_IMR     0x0F /* page0 write: interrupt mask */
#define REG_DATA    0x10 /* remote DMA data port, read or write */

/* CR bits */
#define CR_STP      0x01 /* stop */
#define CR_STA      0x02 /* start */
#define CR_TXP      0x04 /* transmit packet */
#define CR_RD_READ  0x08 /* remote DMA: read */
#define CR_RD_WRITE 0x10 /* remote DMA: write */
#define CR_RD_ABORT 0x20 /* remote DMA: abort/complete, "no DMA" */
#define CR_PAGE0    0x00
#define CR_PAGE1    0x40

/* ISR bits */
#define ISR_PRX     0x01 /* packet received */
#define ISR_PTX     0x02 /* packet transmitted */
#define ISR_RDC     0x40 /* remote DMA complete */
#define ISR_RST     0x80 /* reset status */

/* Real NE2000 page layout (256-byte pages inside the card's own internal
   memory): 6 pages for one TX buffer (1536 bytes, comfortably over
   Ethernet's 1514-byte max frame), the rest of the classic 8KB working set
   for the RX ring. These exact numbers (0x40/0x46/0x60) are the standard
   values cited across NE2000 driver prior art (OSDev, ToaruOS, Minix),
   not arbitrary. */
#define TX_START_PAGE 0x40
#define RX_START_PAGE 0x46
#define RX_STOP_PAGE  0x60
#define TX_BUF_MAX    ((RX_START_PAGE - TX_START_PAGE) * 256) /* 1536 bytes */

static u16 io_base = 0;
static u8  rx_bnry = RX_START_PAGE; /* our RX read cursor, mirrors REG_BNRY */
static u8  our_mac[6];

/* Blocks until the remote-DMA-complete bit lands in the ISR (set after any
   remote read/write finishes), then clears it. Every remote DMA op below
   needs this before touching the registers it just used, or the next
   command can race the card's own completion. Timeout mirrors
   rtl8139.c's other polling loops: a bounded iteration count, not a real
   clock, this is short/local I/O so it's not subject to the same host-CPU-
   scheduling skew net.c's own wait loops had to account for. */
static int wait_rdc(void) {
    int timeout = 1000000;
    while (!(inb(io_base + REG_ISR) & ISR_RDC) && timeout--) {}
    outb(io_base + REG_ISR, ISR_RDC);
    return timeout > 0;
}

/* Remote-DMA read of len bytes starting at the card's internal page-
   addressed offset addr, into buf. Used both for the one-time PROM read
   (MAC address) and per-packet RX ring reads.

   Real bug found live, headless, against v86: this used to walk the data
   port with single-byte inb() calls. DCR (REG_DCR) is programmed 0x49
   below, which sets the word-transfer bit (WTS, bit0), and confirmed
   directly against v86's own NE2000 emulation (libv86.js's data_port_
   read8/data_port_write16) that in word mode, ANY access to the data
   port, even an explicit byte-sized one, internally performs two 8-bit
   transfers and advances the card's own remote-DMA pointer (rsar) by 2,
   not 1: data_port_read8 calls data_port_read16, which in word mode reads
   two bytes and returns only the low one, silently dropping the high
   byte and desyncing the read position by one extra byte on every single
   inb(). The PROM read and every packet read were consuming twice the
   real ring/PROM data per byte actually delivered, which is exactly why
   arp_resolve kept timing out: the ARP reply frame was arriving (RX ring
   non-empty, header read succeeded) but every subsequent inb() read
   garbage, corrupted data instead of the real frame. Real hardware in
   word mode has the identical requirement (this is standard, documented
   8390 word-mode behavior, not a v86-only quirk): the data port must be
   accessed 16 bits at a time. Fixed by using inw()/outw() here, pairing
   two bytes per port access, with an odd trailing byte simply padded (the
   ring/PROM's own length field, not this loop, decides what's real). */
static int remote_read(u16 addr, void *buf, u32 len) {
    outb(io_base + REG_RBCR0, (u8)(len & 0xFF));
    outb(io_base + REG_RBCR1, (u8)((len >> 8) & 0xFF));
    outb(io_base + REG_RSAR0, (u8)(addr & 0xFF));
    outb(io_base + REG_RSAR1, (u8)((addr >> 8) & 0xFF));
    outb(io_base + REG_CR, CR_PAGE0 | CR_RD_READ | CR_STA);
    u8 *out = buf;
    for (u32 i = 0; i < len; i += 2) {
        u16 w = inw(io_base + REG_DATA);
        out[i] = (u8)(w & 0xFF);
        if (i + 1 < len) out[i + 1] = (u8)(w >> 8);
    }
    return wait_rdc();
}

/* Remote-DMA write of len bytes into the card's internal page-addressed
   offset addr, from buf. Used for TX: the frame is written into the TX
   buffer page before the driver tells the card to actually send it. Same
   word-mode fix as remote_read above, and for the identical reason (DCR's
   word-transfer bit means every data-port access, byte-sized or not,
   actually moves two bytes). */
static int remote_write(u16 addr, const void *buf, u32 len) {
    outb(io_base + REG_RBCR0, (u8)(len & 0xFF));
    outb(io_base + REG_RBCR1, (u8)((len >> 8) & 0xFF));
    outb(io_base + REG_RSAR0, (u8)(addr & 0xFF));
    outb(io_base + REG_RSAR1, (u8)((addr >> 8) & 0xFF));
    outb(io_base + REG_CR, CR_PAGE0 | CR_RD_WRITE | CR_STA);
    const u8 *src = buf;
    for (u32 i = 0; i < len; i += 2) {
        u16 w = src[i];
        if (i + 1 < len) w |= (u16)src[i + 1] << 8;
        outw(io_base + REG_DATA, w);
    }
    return wait_rdc();
}

int ne2k_init(void) {
    struct pci_device dev;
    /* vendor 0x10EC / device 0x8029: the RTL8029 (NE2000-compatible) clone
       QEMU and v86 both emulate as "ne2k" -- confirmed straight out of
       v86's own libv86.js pci_space table (this.pci_space=[236,16,41,
       128,...], i.e. bytes 0xEC,0x10,0x29,0x80, vendor/device little-
       endian), not assumed from general NE2000 datasheet knowledge. Class/
       subclass alone (0x02/0x00) can't tell this apart from RTL8139, which
       shares it, hence pci_find_device_vid rather than pci_find_device. */
    if (!pci_find_device_vid(0x10EC, 0x8029, &dev)) return 0;
    if (!dev.bar0_is_io) return 0; /* NE2000's BAR0 is always I/O space */
    io_base = (u16)dev.bar0;

    pci_enable_device(&dev);

    /* Standard 8390 reset+init sequence (OSDev "Ne2000", ToaruOS's
       ne2000.c): stop the card, program DCR/RBCR/RCR/TCR, set up the RX
       ring page range, clear/mask interrupts (polled driver, no IRQ
       handler wired up, same choice rtl8139.c made), read the PROM for
       the real MAC, then switch RCR to its real running mode and start
       the card. */
    outb(io_base + REG_CR, CR_PAGE0 | CR_RD_ABORT | CR_STP);
    outb(io_base + REG_DCR, 0x49);  /* word-wide transfers, normal FIFO threshold, no loopback */
    outb(io_base + REG_RBCR0, 0);
    outb(io_base + REG_RBCR1, 0);
    outb(io_base + REG_RCR, 0x20);  /* monitor mode (don't accept anything) while we finish setup */
    outb(io_base + REG_TCR, 0x02);  /* internal loopback during setup, same reasoning as monitor mode above */
    outb(io_base + REG_PSTART, RX_START_PAGE);
    outb(io_base + REG_BNRY, RX_START_PAGE);
    outb(io_base + REG_PSTOP, RX_STOP_PAGE);
    outb(io_base + REG_ISR, 0xFF);  /* clear every latched ISR bit */
    outb(io_base + REG_IMR, 0x00);  /* polled, no interrupts unmasked */

    /* Real MAC lives in the card's PROM, read via one remote-DMA read of
       the first 32 bytes at internal address 0, exactly like every other
       NE2000-family driver does it (this is the documented, well-cited
       approach, not a guess): each address's byte is duplicated
       (prom[i*2] holds the real byte, prom[i*2+1] is the duplicate) since
       the PROM is wired for a 16-bit-wide access pattern even though the
       remote-DMA read here pulls it one byte at a time. */
    u8 prom[32];
    remote_read(0, prom, sizeof(prom));
    for (int i = 0; i < 6; i++) our_mac[i] = prom[i * 2];

    /* Program PAR0-5 (page1) with the MAC just read, and CURR (page1,
       the NIC's own RX write cursor) to one past PSTART. This is the
       standard init even though v86's emulated receive path filters by
       its own internal mac[] array rather than these registers (checked
       directly against libv86.js's Mc.prototype.receive), since real
       RTL8029/NE2000 hardware does depend on PAR being set correctly. */
    outb(io_base + REG_CR, CR_PAGE1 | CR_RD_ABORT | CR_STP);
    for (int i = 0; i < 6; i++) outb(io_base + REG_PAR0 + i, our_mac[i]);
    outb(io_base + REG_CURR, RX_START_PAGE + 1);
    outb(io_base + REG_CR, CR_PAGE0 | CR_RD_ABORT | CR_STP);

    outb(io_base + REG_TCR, 0x00);  /* real running mode: normal operation, no loopback */
    outb(io_base + REG_RCR, 0x1C);  /* promiscuous | accept multicast | accept broadcast: same "accept everything" choice rtl8139_init makes (0x0F there) */
    outb(io_base + REG_TPSR, TX_START_PAGE);
    outb(io_base + REG_CR, CR_PAGE0 | CR_RD_ABORT | CR_STA); /* start the card for real */

    rx_bnry = RX_START_PAGE;
    return 1;
}

void ne2k_get_mac(u8 mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = our_mac[i];
}

/* Pure logic, no hardware I/O, unit-testable the same way
   rtl8139_clamp_len is (see rtl8139.c and its own comment on the real OOB
   bug this exact pattern was written to prevent from recurring): given
   the NIC's own raw packet-header length (which includes the ring's own
   4-byte per-packet header, unlike rtl8139's length which only counts the
   CRC) and the caller's buffer size, returns how many bytes ne2k_receive
   will actually copy into that buffer. Never trust the wire/ring length
   past what the caller's own buffer can hold, and never let the "subtract
   the 4-byte ring header" step underflow on a corrupt/runt length. */
static u32 ne2k_clamp_len(u16 length, u32 maxlen) {
    u32 data_len = length >= 4 ? (u32)length - 4 : 0; /* guard the ring-header-strip underflow on a corrupt/runt length */
    return data_len < maxlen ? data_len : maxlen;
}

int ne2k_clamp_selftest(void) {
    if (ne2k_clamp_len(9000, 1514) != 1514) return 0; /* oversized claim must clamp to what's actually copied */
    if (ne2k_clamp_len(64, 1514) != 60) return 0;      /* honest small frame: 64 - 4 (ring header) = 60 */
    if (ne2k_clamp_len(2, 1514) != 0) return 0;        /* runt shorter than the ring header itself: no underflow */
    return 1;
}

/* RX ring packet header (4 bytes, written by the NIC at the start of every
   received frame inside the ring): [status][next_page][length_lo][length_hi].
   length includes this 4-byte header itself, same "envelope carries its
   own header in the length" shape rtl8139's ring has, just a different
   byte layout. */
u32 ne2k_receive(void *buf, u32 maxlen) {
    outb(io_base + REG_CR, CR_PAGE1 | CR_RD_ABORT | CR_STA);
    u8 curr = inb(io_base + REG_CURR);
    outb(io_base + REG_CR, CR_PAGE0 | CR_RD_ABORT | CR_STA);

    /* Real off-by-one bug, found live against v86's own NE2000 emulation
       (libv86.js's Mc.prototype.receive writes each incoming packet
       starting AT curpg, the page CURR pointed to *before* this packet
       arrived, then advances CURR past it): the ring's oldest unread
       packet lives at page (BNRY + 1), never at BNRY itself. BNRY is "one
       page behind the next unread packet" by definition, standard 8390
       semantics documented the same way in OSDev's Ne2000 driver
       reference; this used to compare curr against rx_bnry directly and
       read the header from rx_bnry*256, both off by one page. Confirmed
       live: rx_bnry started at RX_START_PAGE (0x46) and CURR at 0x47 (one
       page ahead, the correct init gap), so the old check (curr==rx_bnry)
       reported "non-empty" from the very first poll, before any real
       packet had arrived, and the header read landed on an unused page
       (status=0x00, next=0x00, len=0x0000, exactly the all-zero page
       nothing had ever written to), silently discarding every real ARP
       reply that then arrived one page later. */
    u8 next_unread = (u8)(rx_bnry + 1 == RX_STOP_PAGE ? RX_START_PAGE : rx_bnry + 1);
    if (curr == next_unread) return 0; /* ring genuinely empty: NIC's write cursor caught up to our read cursor */

    u8 hdr[4];
    if (!remote_read((u16)next_unread * 256, hdr, sizeof(hdr))) return 0;
    u8 status = hdr[0];
    u8 next_page = hdr[1];
    u16 length = (u16)hdr[2] | ((u16)hdr[3] << 8);

    /* next_page (from the packet's own ring header) is the page of the
       NEXT unread packet after this one, i.e. exactly what the hardware
       BNRY register should become one page BEHIND: rx_bnry (our software
       mirror of BNRY) always holds "one page behind the next unread
       packet", never the next unread packet's own page (see the comment
       above on next_unread). */
    u8 new_bnry = (u8)(next_page == RX_START_PAGE ? RX_STOP_PAGE - 1 : next_page - 1);

    if (!(status & 0x01)) { /* PRX bit not set in this packet's own header: don't trust it, same discipline rtl8139_receive uses on its own status word */
        rx_bnry = new_bnry; /* still had to advance past it, or the ring never drains */
        outb(io_base + REG_BNRY, rx_bnry);
        return 0;
    }

    u32 copy_len = ne2k_clamp_len(length, maxlen);
    /* Real hardware auto-wraps a remote-DMA read that crosses PSTOP back
       to PSTART (documented 8390 ring behavior, the same reason the ring
       is a ring at all); one single remote_read of the clamped length is
       enough, no manual wrap-handling needed here. */
    if (copy_len > 0) {
        if (!remote_read((u16)next_unread * 256 + 4, buf, copy_len)) return 0;
    }

    rx_bnry = new_bnry;
    outb(io_base + REG_BNRY, rx_bnry);
    outb(io_base + REG_ISR, ISR_PRX); /* write-1-to-clear, same convention rtl8139_receive follows on its own ISR */

    return copy_len;
}

int ne2k_send(const void *data, u32 len) {
    if (len > TX_BUF_MAX) return 0;

    if (!remote_write((u16)TX_START_PAGE * 256, data, len)) return 0;

    outb(io_base + REG_TPSR, TX_START_PAGE);
    outb(io_base + REG_TBCR0, (u8)(len & 0xFF));
    outb(io_base + REG_TBCR1, (u8)((len >> 8) & 0xFF));
    outb(io_base + REG_ISR, ISR_PTX); /* clear any stale PTX before starting a fresh send, so the poll below can't see a leftover bit from last time */
    outb(io_base + REG_CR, CR_PAGE0 | CR_RD_ABORT | CR_STA | CR_TXP); /* fire the transmit */

    int timeout = 1000000;
    while (!(inb(io_base + REG_ISR) & ISR_PTX) && timeout--) {}
    if (timeout > 0) outb(io_base + REG_ISR, ISR_PTX);
    return timeout > 0;
}
