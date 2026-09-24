/* Intel 82540EM (e1000) NIC driver. Register layout and the reset/init
   sequence follow Intel's own PCI/PCIe GbE Controllers Software Developer's
   Manual (the 8254x family shares one register map), the same document
   every hobby-OS e1000 driver implements against. Targets QEMU's
   `-device e1000`, which emulates this exact chip (PCI vendor 0x8086,
   device 0x100E), so this driver should also carry over to real hardware
   and the many 8254x-compatible NICs still common on real PCs -- unlike
   RTL8139/NE2000, both long obsolete on real machines, an Intel or
   Realtek chip is what most PCs from the last 15 years actually ship. */
#include "e1000.h"
#include "pci.h"
#include "paging.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E /* 82540EM, QEMU's `-device e1000` model */

/* Register offsets, relative to the MMIO BAR (BAR0, always memory-space
   for this chip, unlike RTL8139's I/O-space BAR0). */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_RST   (1u << 26)
#define CTRL_ASDE  (1u << 5)
#define CTRL_SLU   (1u << 6)  /* set link up */

#define EERD_START (1u << 0)
#define EERD_DONE  (1u << 4) /* 82540EM's own bit position, not the older 82544 layout */

#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15) /* accept broadcast */
#define RCTL_BSIZE_2048 (0u << 16)
#define RCTL_SECRC  (1u << 26) /* strip Ethernet CRC before it reaches the ring */

#define TCTL_EN   (1u << 1)
#define TCTL_PSP  (1u << 3)
#define TCTL_CT_DEFAULT   (0x0F << 4)
#define TCTL_COLD_DEFAULT (0x40 << 12)

#define RXD_STAT_DD  0x01
#define TXD_STAT_DD  0x01
#define TXD_CMD_EOP  0x01
#define TXD_CMD_IFCS 0x02
#define TXD_CMD_RS   0x08

/* Same higher-half DMA gap RTL8139 hits (drivers/rtl8139.c's own
   KVIRT_TO_PHYS comment explains it in full): the NIC does raw physical
   DMA and has no notion of the CPU's page tables, so descriptor/buffer
   addresses handed to the ring have to be physical, not whatever &buffer
   reads as from higher-half kernel C code. The MMIO BAR itself is
   different: that's a real device address paging_map_region() identity-
   maps, so register reads/writes below use the physical BAR address
   directly, unmodified. */
#define KVIRT_TO_PHYS(addr) ((u32)(addr) - 0xC0000000)

#define NUM_RX_DESC 8
#define NUM_TX_DESC 8
#define RXBUF_SIZE  2048

struct e1000_rx_desc {
    u32 addr_lo;
    u32 addr_hi;
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
} __attribute__((packed));

struct e1000_tx_desc {
    u32 addr_lo;
    u32 addr_hi;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} __attribute__((packed));

static u32 mmio_base = 0;
static int rx_cur = 0;
static int tx_cur = 0;

static struct e1000_rx_desc rx_ring[NUM_RX_DESC] __attribute__((aligned(16)));
static struct e1000_tx_desc tx_ring[NUM_TX_DESC] __attribute__((aligned(16)));
static u8 rx_buffers[NUM_RX_DESC][RXBUF_SIZE] __attribute__((aligned(16)));
static u8 tx_buffers[NUM_TX_DESC][1792] __attribute__((aligned(16)));

static inline u32 mmio_read32(u32 reg) {
    return *(volatile u32 *)(mmio_base + reg);
}
static inline void mmio_write32(u32 reg, u32 val) {
    *(volatile u32 *)(mmio_base + reg) = val;
}

/* EEPROM read, the real-hardware-accurate way to get the burned-in MAC
   (rather than trusting RAL/RAH already being preloaded, which QEMU does
   as a convenience but real firmware-less boot can't assume). 82540EM's
   EERD register: write the word address in bits 8-15 plus START (bit0),
   poll DONE (bit4, this chip's own position -- the older 82544 uses bit1,
   a real 8254x-family trap this constant name calls out on purpose), then
   read the 16-bit word back out of bits 16-31. */
static u16 e1000_eeprom_read(u8 addr) {
    mmio_write32(REG_EERD, ((u32)addr << 8) | EERD_START);
    u32 val;
    int timeout = 100000;
    do {
        val = mmio_read32(REG_EERD);
    } while (!(val & EERD_DONE) && --timeout);
    return (u16)(val >> 16);
}

static u8 nic_mac[6];

int e1000_init(void) {
    struct pci_device dev;
    if (!pci_find_device_vid(E1000_VENDOR_ID, E1000_DEVICE_ID, &dev)) return 0;
    if (dev.bar0_is_io) return 0; /* e1000's BAR0 is always memory-space, unlike RTL8139's */

    pci_enable_device(&dev);

    mmio_base = dev.bar0;
    if (!paging_map_region(mmio_base, 0x20000)) return 0; /* 128KB covers the whole 82540EM register file */

    /* Software reset, then wait for the chip to clear the RST bit itself
       (same shape as RTL8139's own reset wait). */
    mmio_write32(REG_CTRL, mmio_read32(REG_CTRL) | CTRL_RST);
    int timeout = 1000000;
    while ((mmio_read32(REG_CTRL) & CTRL_RST) && --timeout) {}
    if (mmio_read32(REG_CTRL) & CTRL_RST) return 0; /* check the real bit, not just whether the budget ran out */

    mmio_write32(REG_IMS, 0); /* polled, no IRQ wiring yet -- same call rtl8139.c makes, see e1000.h */
    mmio_read32(REG_ICR);     /* clear any pending interrupt-cause bits left over from reset */

    mmio_write32(REG_CTRL, mmio_read32(REG_CTRL) | CTRL_ASDE | CTRL_SLU);

    /* MAC from the EEPROM: three 16-bit words, little-endian within each
       word, exactly how the chip's own PROM lays out a 48-bit MAC. */
    u16 w0 = e1000_eeprom_read(0);
    u16 w1 = e1000_eeprom_read(1);
    u16 w2 = e1000_eeprom_read(2);
    nic_mac[0] = (u8)(w0 & 0xFF); nic_mac[1] = (u8)(w0 >> 8);
    nic_mac[2] = (u8)(w1 & 0xFF); nic_mac[3] = (u8)(w1 >> 8);
    nic_mac[4] = (u8)(w2 & 0xFF); nic_mac[5] = (u8)(w2 >> 8);
    /* Program RAL0/RAH0 with the same address plus the valid bit (bit31 of
       RAH), the standard "receive address 0 = our own unicast MAC" setup
       every 8254x driver does so unicast frames actually pass the filter. */
    u32 ral = nic_mac[0] | (nic_mac[1] << 8) | (nic_mac[2] << 16) | (nic_mac[3] << 24);
    u32 rah = nic_mac[4] | (nic_mac[5] << 8) | (1u << 31);
    mmio_write32(REG_RAL0, ral);
    mmio_write32(REG_RAH0, rah);

    /* RX ring: NUM_RX_DESC descriptors, each pointing at its own 2048-byte
       buffer. Physical addresses throughout, same DMA-gap reasoning as
       RTL8139's rx_buffer. */
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_ring[i].addr_lo = KVIRT_TO_PHYS(rx_buffers[i]);
        rx_ring[i].addr_hi = 0;
        rx_ring[i].status = 0;
    }
    rx_cur = 0;
    mmio_write32(REG_RDBAL, KVIRT_TO_PHYS(rx_ring));
    mmio_write32(REG_RDBAH, 0);
    mmio_write32(REG_RDLEN, NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    mmio_write32(REG_RDH, 0);
    mmio_write32(REG_RDT, NUM_RX_DESC - 1); /* tail = last free descriptor, standard ring convention */
    mmio_write32(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);

    /* TX ring: NUM_TX_DESC descriptors, each with its own send buffer, same
       "cycle through in order" discipline RTL8139's tx_buffer[4] uses --
       reusing descriptor 0 for every send risks the same DD-never-clears
       stall RTL8139 hit if a previous send's DMA hasn't retired yet. */
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_ring[i].addr_lo = KVIRT_TO_PHYS(tx_buffers[i]);
        tx_ring[i].addr_hi = 0;
        tx_ring[i].status = TXD_STAT_DD; /* mark idle/available up front */
    }
    tx_cur = 0;
    mmio_write32(REG_TDBAL, KVIRT_TO_PHYS(tx_ring));
    mmio_write32(REG_TDBAH, 0);
    mmio_write32(REG_TDLEN, NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    mmio_write32(REG_TDH, 0);
    mmio_write32(REG_TDT, 0);
    mmio_write32(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT_DEFAULT | TCTL_COLD_DEFAULT);
    mmio_write32(REG_TIPG, 0x0060200A); /* Intel's documented default inter-packet-gap timings */

    return 1;
}

void e1000_get_mac(u8 mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = nic_mac[i];
}

/* Same clamp discipline as rtl8139_clamp_len/ne2k_clamp_len: never trust
   the card's own reported length past the caller's buffer, so a corrupt or
   oversized length field can't turn into an OOB write one layer up in
   net.c. e1000's RX descriptor length already excludes the CRC (RCTL's
   SECRC strips it before the ring), unlike RTL8139's raw ring format, so
   there's no "-4" here -- just the same min-with-maxlen clamp. */
static u32 e1000_clamp_len(u16 length, u32 maxlen) {
    return length < maxlen ? length : maxlen;
}

int e1000_clamp_selftest(void) {
    if (e1000_clamp_len(9000, 1514) != 1514) return 0; /* oversized claim must clamp to what's actually copied */
    if (e1000_clamp_len(60, 1514) != 60) return 0;      /* honest small frame passes through unclamped */
    if (e1000_clamp_len(0, 1514) != 0) return 0;        /* empty claim stays zero, no underflow to worry about here */
    return 1;
}

unsigned int e1000_receive(void *buf, unsigned int maxlen) {
    struct e1000_rx_desc *desc = &rx_ring[rx_cur];
    if (!(desc->status & RXD_STAT_DD)) return 0; /* nothing new since the last poll */

    u32 copy_len = e1000_clamp_len(desc->length, maxlen);
    u8 *out = buf;
    u8 *src = rx_buffers[rx_cur];
    for (u32 i = 0; i < copy_len; i++) out[i] = src[i];

    desc->status = 0; /* hand this descriptor back to the card */
    mmio_write32(REG_RDT, rx_cur); /* tail follows the descriptor we just freed */
    rx_cur = (rx_cur + 1) % NUM_RX_DESC;

    return copy_len;
}

int e1000_send(const void *data, unsigned int len) {
    if (len > sizeof(tx_buffers[0])) return 0;

    struct e1000_tx_desc *desc = &tx_ring[tx_cur];
    /* Same "descriptor still in flight" hazard RTL8139's 4-buffer TX ring
       exists to avoid: wait for the previous occupant of this slot to
       finish before overwriting its buffer. On a fresh ring this is
       already true (init sets every descriptor's DD bit). */
    int timeout = 1000000;
    while (!(desc->status & TXD_STAT_DD) && --timeout) {}
    if (!(desc->status & TXD_STAT_DD)) return 0; /* check the real bit, not just whether the budget ran out */

    const u8 *src = data;
    u8 *dst = tx_buffers[tx_cur];
    for (u32 i = 0; i < len; i++) dst[i] = src[i];

    desc->length = (u16)len;
    desc->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    desc->status = 0;

    int next = (tx_cur + 1) % NUM_TX_DESC;
    tx_cur = next;
    mmio_write32(REG_TDT, (u32)next); /* tail = next free slot, starts the transmit of the one before it */

    /* Check the status bit itself after the wait, not just whether the
       budget ran out -- DD can land on the very last iteration, and a
       loop that only trusts "timeout > 0" throws that real success away
       (found live: status came back 0x01/DD-set with timeout already at
       0, which the old `return timeout > 0` reported as a failed send). */
    timeout = 1000000;
    while (!(desc->status & TXD_STAT_DD) && --timeout) {}
    return (desc->status & TXD_STAT_DD) != 0;
}
