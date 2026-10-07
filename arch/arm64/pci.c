/* M4: PCI Express, just enough to reach a USB controller.

   Two hosts, one enumerator. QEMU's virt machine has a standard ECAM window (config space for every bus, device and
   function at a fixed address). A Raspberry Pi 4 has Broadcom's own root complex at 0xFD500000: the root port's
   config space is its register block, and everything below it goes through an index register and a 4 KiB data
   window. Either way scan_bus() walks the tree, numbers the buses behind bridges, gives every memory BAR an address
   from the host's outbound window and turns on memory decode and bus mastering. pci_xhci_base() hands back the CPU
   address of the first xHCI controller's registers (class 0x0C0330), or 0.

   Prior art: the OSDev wiki's "PCI" and "PCI Express" pages for the enumerator; the Pi 4 bring-up follows Circle's
   lib/bcmpciehostbridge.cpp (rsta2/circle), itself a port of Linux drivers/pci/controller/pcie-brcmstb.c. */
#define R32(a) (*(volatile unsigned *)(unsigned long)(a))
void kputs(const char *s);
void kdec(unsigned v);
void kx(unsigned v);


struct pci_host {
    unsigned long (*cfg)(unsigned bus, unsigned dev, unsigned fn);   /* config space of one function, 0 if unreachable */
    unsigned long mem_bus, mem_cpu, mem_size;                         /* the outbound memory window, PCI side and CPU side */
    int dev0_only;                                                    /* below a PCIe root port only device 0 exists */
};
static const struct pci_host *host;

static unsigned cfg_rd(unsigned bus, unsigned dev, unsigned fn, unsigned off) {
    unsigned long c = host->cfg(bus, dev, fn);
    return c ? R32(c + off) : 0xFFFFFFFFu;
}
static void cfg_wr(unsigned bus, unsigned dev, unsigned fn, unsigned off, unsigned v) {
    unsigned long c = host->cfg(bus, dev, fn);
    if (c) R32(c + off) = v;
}

#ifndef PI_BUILD
/* QEMU virt: with its default highmem layout the ECAM sits at 256 GiB + 256 MiB (main.c maps that GiB as device
   memory), 256 buses of 1 MiB. The 32-bit memory window is 0x10000000 to 0x3EFEFFFF, the same address both sides. */
#define ECAM 0x4010000000UL
static unsigned long ecam_cfg(unsigned bus, unsigned dev, unsigned fn) {
    return ECAM + ((unsigned long)bus << 20) + ((unsigned long)dev << 15) + ((unsigned long)fn << 12);
}
static const struct pci_host virt_host = { ecam_cfg, 0x10000000UL, 0x10000000UL, 0x2EFF0000UL, 0 };
#else
/* ---- The Raspberry Pi 4 (BCM2711) root complex. UNTESTED on a real board: QEMU's raspi4b has no PCIe.
   Register offsets and the sequence are Circle's bcmpciehostbridge.cpp (the RASPPI == 4 paths) and Linux's
   pcie-brcmstb.c. Outbound: CPU 0x6_0000_0000 is PCI 0xF8000000, 64 MiB, as in the Pi 4 device tree. Inbound: PCI
   address 0 is CPU address 0 for the first 1 GiB, which holds the kernel and its heap, so every DMA address the
   xHCI driver hands out is just the physical address. ---- */
#define PCIE 0xFD500000UL
#define PCIE_CAP 0x00ac                    /* the root port's PCI Express capability */
#define RC_CFG_VENDOR_SPECIFIC_REG1 0x0188
#define RC_CFG_PRIV1_ID_VAL3 0x043c
#define MISC_MISC_CTRL 0x4008
#define MISC_CPU_2_PCIE_MEM_WIN0_LO 0x400c
#define MISC_CPU_2_PCIE_MEM_WIN0_HI 0x4010
#define MISC_RC_BAR1_CONFIG_LO 0x402c
#define MISC_RC_BAR2_CONFIG_LO 0x4034
#define MISC_RC_BAR2_CONFIG_HI 0x4038
#define MISC_RC_BAR3_CONFIG_LO 0x403c
#define MISC_PCIE_STATUS 0x4068
#define MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT 0x4070
#define MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI 0x4080
#define MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI 0x4084
#define MISC_HARD_PCIE_HARD_DEBUG 0x4204
#define INTR2_CPU_BASE 0x4300
#define EXT_CFG_DATA 0x8000
#define EXT_CFG_INDEX 0x9000
#define RGR1_SW_INIT_1 0x9210
#define OUT_CPU 0x600000000UL
#define OUT_PCI 0xF8000000UL
#define OUT_SIZE 0x4000000UL
#define DMA_SIZE_LOG2 30                    /* inbound window: 1 GiB at PCI 0 = CPU 0 */
int mbox_notify_xhci_reset(unsigned dev_addr);   /* main.c: VideoCore mailbox tag 0x00030058 */
static unsigned long now(void) { unsigned long t; __asm__ volatile ("isb\n mrs %0, cntpct_el0" : "=r"(t)); return t; }
static void udelay(unsigned long us) {
    unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 54000000;
    unsigned long t0 = now(), n = f / 1000000 * us;
    while (now() - t0 < n) {}
}

static void fld(unsigned off, unsigned mask, int shift, unsigned v) {   /* read, change one field, write, read back */
    unsigned r = R32(PCIE + off);
    R32(PCIE + off) = (r & ~mask) | ((v << shift) & mask);
    (void)R32(PCIE + off);
}
static unsigned long brcm_cfg(unsigned bus, unsigned dev, unsigned fn) {
    if (bus == 0) return dev || fn ? 0 : PCIE;   /* the root port is the register block itself */
    R32(PCIE + EXT_CFG_INDEX) = (bus << 20) | (dev << 15) | (fn << 12);
    return PCIE + EXT_CFG_DATA;
}
static const struct pci_host pi_host = { brcm_cfg, OUT_PCI, OUT_CPU, OUT_SIZE, 1 };
static int brcm_link_up(void) { unsigned s = R32(PCIE + MISC_PCIE_STATUS); return (s & 0x20) && (s & 0x10); }   /* DL active, PHY link up */

extern volatile int kprobe_armed, kprobe_faulted;   /* main.c: a data abort while armed is skipped, not fatal */
static int brcm_pcie_init(void) {
    /* QEMU's raspi4b has no PCIe and faults on the first access; a real Pi answers. Probe the reset register first. */
    kprobe_faulted = 0; kprobe_armed = 1;
    (void)R32(PCIE + RGR1_SW_INIT_1);
    kprobe_armed = 0;
    if (kprobe_faulted) { kputs("usb pcie absent\n"); return 0; }
    kputs("usb pcie reset\n");
    fld(RGR1_SW_INIT_1, 0x2, 1, 1);                       /* bridge into reset */
    fld(RGR1_SW_INIT_1, 0x1, 0, 1);                       /* assert PERST# */
    udelay(200);
    fld(RGR1_SW_INIT_1, 0x2, 1, 0);                       /* bridge out of reset */
    fld(MISC_HARD_PCIE_HARD_DEBUG, 0x08000000, 27, 0);    /* SerDes out of IDDQ (power down) */
    udelay(200);
    unsigned c = R32(PCIE + MISC_MISC_CTRL);               /* SCB access on, config reads of nothing answer all-ones, 128-byte bursts */
    c |= 1u << 12; c |= 1u << 13; c &= ~0x300000u;
    R32(PCIE + MISC_MISC_CTRL) = c;
    R32(PCIE + MISC_RC_BAR2_CONFIG_LO) = 0 | (DMA_SIZE_LOG2 - 15);   /* inbound at PCI 0, size code: log2 - 15 for 64 KiB..32 GiB */
    R32(PCIE + MISC_RC_BAR2_CONFIG_HI) = 0;
    fld(MISC_MISC_CTRL, 0xf8000000, 27, DMA_SIZE_LOG2 - 15);         /* SCB0 size to match */
    fld(MISC_RC_BAR1_CONFIG_LO, 0x1f, 0, 0);              /* no PCIe-to-GISB window */
    fld(MISC_RC_BAR3_CONFIG_LO, 0x1f, 0, 0);              /* no PCIe-to-SCB window */
    R32(PCIE + INTR2_CPU_BASE + 0x8) = 0xffffffff;        /* clear stale interrupts, then mask them all: we poll */
    (void)R32(PCIE + INTR2_CPU_BASE + 0x8);
    R32(PCIE + INTR2_CPU_BASE + 0x10) = 0xffffffff;
    (void)R32(PCIE + INTR2_CPU_BASE + 0x10);
    unsigned lc = R32(PCIE + PCIE_CAP + 0x0c);             /* limit to gen 2, as Circle and Linux do on this board */
    R32(PCIE + PCIE_CAP + 0x0c) = (lc & ~0xfu) | 2;
    unsigned l2 = R32(PCIE + PCIE_CAP + 0x30);
    R32(PCIE + PCIE_CAP + 0x30) = (l2 & ~0xfu) | 2;
    fld(RGR1_SW_INIT_1, 0x1, 0, 0);                       /* release PERST# */
    udelay(100000);                                       /* 100 ms, PCIe CEM 2.2 */
    for (int ms = 0; ms < 100 && !brcm_link_up(); ms += 5) udelay(5000);
    if (!brcm_link_up()) { kputs("usb pcie link down\n"); return 0; }
    if (!(R32(PCIE + MISC_PCIE_STATUS) & 0x80)) { kputs("usb pcie in endpoint mode\n"); return 0; }
    /* outbound window 0 */
    R32(PCIE + MISC_CPU_2_PCIE_MEM_WIN0_LO) = (unsigned)OUT_PCI;
    R32(PCIE + MISC_CPU_2_PCIE_MEM_WIN0_HI) = (unsigned)(OUT_PCI >> 32);
    unsigned long base_mb = OUT_CPU >> 20, limit_mb = (OUT_CPU + OUT_SIZE - 1) >> 20;
    fld(MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT, 0xfff0, 4, (unsigned)base_mb);
    fld(MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT, 0xfff00000, 20, (unsigned)limit_mb);
    fld(MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI, 0xff, 0, (unsigned)(base_mb >> 12));
    fld(MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI, 0xff, 0, (unsigned)(limit_mb >> 12));
    fld(RC_CFG_PRIV1_ID_VAL3, 0xffffff, 0, 0x060400);     /* the root port reports itself as a PCI-to-PCI bridge */
    fld(RC_CFG_VENDOR_SPECIFIC_REG1, 0xc, 2, 0);          /* little-endian inbound data */
    fld(MISC_HARD_PCIE_HARD_DEBUG, 0x2, 1, 1);            /* gate refclk with CLKREQ# */
    unsigned st = R32(PCIE + PCIE_CAP + 0x10) >> 16;      /* link status */
    kputs("usb pcie link up, gen "); kdec(st & 0xf); kputs(" x"); kdec((st >> 4) & 0x3f); kputs("\n");
    return 1;
}
#endif

/* ---- The enumerator ---- */
static unsigned next_bus;
static unsigned long mem_next, mem_end;   /* PCI-side addresses */
static unsigned long xhci_cpu;

static void assign_bars(unsigned bus, unsigned dev, unsigned fn) {
    for (unsigned off = 0x10; off <= 0x24; off += 4) {
        unsigned orig = cfg_rd(bus, dev, fn, off);
        if (orig & 1) continue;                                   /* I/O space: nothing here needs it */
        int is64 = ((orig >> 1) & 3) == 2;
        cfg_wr(bus, dev, fn, off, 0xFFFFFFFFu);
        unsigned lo = cfg_rd(bus, dev, fn, off) & ~0xFu, hi = 0xFFFFFFFFu;
        if (is64) { cfg_wr(bus, dev, fn, off + 4, 0xFFFFFFFFu); hi = cfg_rd(bus, dev, fn, off + 4); }
        unsigned long size = ~(((unsigned long)hi << 32) | lo) + 1;
        if (!lo && !(is64 && hi)) { if (is64) off += 4; continue; } /* not implemented */
        unsigned long a = (mem_next + size - 1) & ~(size - 1);
        if (a + size > mem_end) { kputs("usb pci out of window space\n"); a = 0; }
        else mem_next = a + size;
        cfg_wr(bus, dev, fn, off, (unsigned)a | (orig & 0xF));
        if (is64) { cfg_wr(bus, dev, fn, off + 4, (unsigned)(a >> 32)); off += 4; }
        if (off == 0x10 || (is64 && off == 0x14)) {               /* BAR0: remember it for the xHCI check below */
            if (!xhci_cpu && a && (cfg_rd(bus, dev, fn, 0x08) >> 8) == 0x0C0330) xhci_cpu = a - host->mem_bus + host->mem_cpu;
        }
    }
}

static void scan_bus(unsigned bus);
static void setup_bridge(unsigned bus, unsigned dev, unsigned fn) {
    unsigned sec = ++next_bus;
    unsigned r18 = cfg_rd(bus, dev, fn, 0x18) & 0xFF000000u;
    cfg_wr(bus, dev, fn, 0x18, r18 | (0xFFu << 16) | (sec << 8) | bus);    /* primary, secondary, subordinate = all for now */
    cfg_wr(bus, dev, fn, 0x1C, (cfg_rd(bus, dev, fn, 0x1C) & 0xFFFF0000u) | 0x00F0);   /* no I/O window */
    cfg_wr(bus, dev, fn, 0x24, 0x0000FFF0);                    /* no prefetchable window */
    cfg_wr(bus, dev, fn, 0x28, 0); cfg_wr(bus, dev, fn, 0x2C, 0);
    mem_next = (mem_next + 0xFFFFF) & ~0xFFFFFUL;              /* bridge windows are 1 MiB granular */
    unsigned long start = mem_next;
    scan_bus(sec);
    mem_next = (mem_next + 0xFFFFF) & ~0xFFFFFUL;
    cfg_wr(bus, dev, fn, 0x18, r18 | (next_bus << 16) | (sec << 8) | bus);
    if (mem_next > start) cfg_wr(bus, dev, fn, 0x20, (unsigned)((((mem_next - 1) >> 16) & 0xFFF0) << 16 | ((start >> 16) & 0xFFF0)));
    else cfg_wr(bus, dev, fn, 0x20, 0x0000FFF0);               /* base above limit: closed */
    cfg_wr(bus, dev, fn, 0x04, (cfg_rd(bus, dev, fn, 0x04) & 0xFFFF) | 0x146);   /* memory, bus master, parity, SERR */
}

static void scan_bus(unsigned bus) {
    unsigned ndev = bus && host->dev0_only ? 1 : 32;
    for (unsigned dev = 0; dev < ndev; dev++) {
        for (unsigned fn = 0; fn < 8; fn++) {
            unsigned id = cfg_rd(bus, dev, fn, 0);
            if ((id & 0xFFFF) == 0xFFFF) { if (!fn) break; continue; }
            unsigned hdr = (cfg_rd(bus, dev, fn, 0x0C) >> 16) & 0xFF, cls = cfg_rd(bus, dev, fn, 0x08) >> 8;
            kputs("usb pci "); kdec(bus); kputs(":"); kdec(dev); kputs("."); kdec(fn); kputs(" ");
            kx(id & 0xFFFF); kputs(":"); kx(id >> 16); kputs(" class "); kx(cls); kputs("\n");
            if ((hdr & 0x7F) == 1) setup_bridge(bus, dev, fn);
            else if ((hdr & 0x7F) == 0) {
                assign_bars(bus, dev, fn);
                cfg_wr(bus, dev, fn, 0x04, (cfg_rd(bus, dev, fn, 0x04) & 0xFFFF) | 0x6);   /* memory decode, bus master */
            }
            if (!fn && !(hdr & 0x80)) break;                  /* not multi-function */
        }
    }
}

unsigned long pci_xhci_base(void) {
#ifndef PI_BUILD
    host = &virt_host;
#else
    host = &pi_host;
    if (!brcm_pcie_init()) return 0;
    /* The VL805 has no firmware of its own on most Pi 4s: the VideoCore loads it once told the controller is out of
       reset. Bus 1 must be routed first, so number the root port's buses, then ask (tag 0x00030058, device 01:00.0). */
    cfg_wr(0, 0, 0, 0x18, (1u << 16) | (1u << 8));
    if (mbox_notify_xhci_reset(1u << 20)) kputs("usb vl805 ok\n");
    else kputs("usb vl805 FAIL: firmware load refused\n");
#endif
    next_bus = 0; xhci_cpu = 0;
    mem_next = host->mem_bus; mem_end = host->mem_bus + host->mem_size;
    scan_bus(0);
    if (xhci_cpu) { kputs("usb xhci at "); kx((unsigned)(xhci_cpu >> 32)); kputs("_"); kx((unsigned)xhci_cpu); kputs("\n"); }
    else kputs("usb no xhci on pci\n");
    return xhci_cpu;
}
