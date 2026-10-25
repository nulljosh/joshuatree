/* virtio.c: moved out of main.c (4.23.2), the god-file split. Part of main.o (MAINSRC), built with main.c's flags; with
   PI_BUILD the same names are the Pi's stand-ins (its network card is the Wi-Fi chip). */
#include "kmain.h"   /* the platform addresses, REG and GICD, and main.c's console and cache helpers */
#include "virtio.h"
void *kmalloc(unsigned int n);

/* ---- M2: devices on QEMU's virt machine. 32 virtio-mmio slots from 0x0A000000, 0x200 apart, each says which device
   sits there: 1 is a network card, 18 is input (keyboard or tablet). Modern virtio (version 2) only: QEMU needs
   -global virtio-mmio.force-legacy=false. Every device talks through queues of buffers the guest lends it. The Pi gets
   USB through xHCI and Ethernet through the Genet MAC instead (M4). ---- */
#ifndef PI_BUILD
#define VIRTIO_BASE 0x0A000000UL
#define VQ 16
struct vq_desc { unsigned long addr; unsigned len; unsigned short flags, next; };
struct vq_avail { unsigned short flags, idx, ring[VQ], used_event; };
struct vq_used { unsigned short flags, idx; struct { unsigned id, len; } ring[VQ]; unsigned short avail_event; };
struct vq { unsigned long base; unsigned q; struct vq_desc *d; struct vq_avail *a; volatile struct vq_used *u; unsigned short seen; };
#define VR(b, o) REG((b) + (o))
static unsigned long vio_find(unsigned id, unsigned long after) {   /* the next slot past `after` holding device `id` */
    for (unsigned long b = after ? after + 0x200 : VIRTIO_BASE; b < VIRTIO_BASE + 32 * 0x200; b += 0x200)
        if (REG(b) == 0x74726976 && REG(b + 4) == 2 && REG(b + 8) == id) return b;   /* "virt", version 2 */
    return 0;
}
static int vio_start(unsigned long b, unsigned features) {   /* reset, then agree on VIRTIO_F_VERSION_1 plus `features` */
    VR(b, 0x70) = 0;
    VR(b, 0x70) = 1 | 2;                            /* acknowledge, driver */
    VR(b, 0x24) = 1; VR(b, 0x20) = 1;               /* features 32..63: VERSION_1 */
    VR(b, 0x24) = 0; VR(b, 0x20) = features & VR(b, 0x10);
    VR(b, 0x70) = 1 | 2 | 8;                        /* features ok */
    return (VR(b, 0x70) & 8) != 0;
}
static int vq_setup(struct vq *v, unsigned long b, unsigned q) {
    VR(b, 0x30) = q;
    if (VR(b, 0x34) < VQ) return 0;
    VR(b, 0x38) = VQ;
    v->base = b; v->q = q; v->seen = 0;
    v->d = kmalloc(sizeof *v->d * VQ); v->a = kmalloc(sizeof *v->a); v->u = kmalloc(sizeof *v->u);
    if (!v->d || !v->a || !v->u) { uart_puts("oom vq\n"); return 0; }
    v->a->flags = 0; v->a->idx = 0; v->u->idx = 0;
    VR(b, 0x80) = (unsigned)(unsigned long)v->d;  VR(b, 0x84) = (unsigned)((unsigned long)v->d >> 32);
    VR(b, 0x90) = (unsigned)(unsigned long)v->a;  VR(b, 0x94) = (unsigned)((unsigned long)v->a >> 32);
    VR(b, 0xA0) = (unsigned)(unsigned long)v->u;  VR(b, 0xA4) = (unsigned)((unsigned long)v->u >> 32);
    VR(b, 0x44) = 1;                                /* queue ready */
    return 1;
}
static void vq_give(struct vq *v, unsigned i, void *buf, unsigned len, int device_writes) {   /* lend descriptor i */
    v->d[i] = (struct vq_desc){ (unsigned long)buf, len, (unsigned short)(device_writes ? 2 : 0), 0 };
    v->a->ring[v->a->idx % VQ] = (unsigned short)i;
    __asm__ volatile ("dsb sy" ::: "memory");
    v->a->idx++;
}
static int vq_take(struct vq *v, unsigned *len) {   /* the next descriptor the device handed back, or -1 */
    if (v->seen == v->u->idx) return -1;
    __asm__ volatile ("dsb sy" ::: "memory");
    unsigned id = v->u->ring[v->seen % VQ].id;
    if (len) *len = v->u->ring[v->seen % VQ].len;
    v->seen++;
    return (int)id;
}
static void vq_kick(struct vq *v) { __asm__ volatile ("dsb sy" ::: "memory"); VR(v->base, 0x50) = v->q; }

/* Keyboard and mouse: one event queue each, lent 8-byte buffers the device fills with Linux evdev events. Both devices
   speak the same events, so one driver serves both. */
#define MAX_INPUT 2
static void gic_enable(unsigned id) {   /* a shared peripheral: on, priority, sent to core 0 */
    *(volatile unsigned char *)(GICD_BASE + 0x400 + id) = 0x80;
    *(volatile unsigned char *)(GICD_BASE + 0x800 + id) = 1;
    GICD(0x100 + 4 * (id / 32)) = 1u << (id % 32);
}
static struct vq vin[MAX_INPUT];
static struct input_event *vin_ev[MAX_INPUT];
static int nvin;
int virtio_input_init(void) {
    for (unsigned long b = vio_find(18, 0); b && nvin < MAX_INPUT; b = vio_find(18, b)) {
        struct vq *v = &vin[nvin];
        struct input_event *ev = kmalloc(sizeof *ev * VQ);
        if (!ev) { uart_puts("oom input\n"); continue; }
        if (!vio_start(b, 0) || !vq_setup(v, b, 0)) continue;
        for (unsigned i = 0; i < VQ; i++) vq_give(v, i, &ev[i], sizeof *ev, 1);
        VR(b, 0x70) = 1 | 2 | 8 | 4;                /* driver ok */
        vq_kick(v);
        gic_enable(48 + (unsigned)((b - VIRTIO_BASE) / 0x200));   /* interrupt when an event lands */
        vin_ev[nvin++] = ev;
    }
    return nvin;
}
/* The interrupt only wakes the core and acknowledges the device; the events are read here, in the main loop. */
void virtio_input_poll(void) {
    for (int n = 0; n < nvin; n++) {
        int id;
        while ((id = vq_take(&vin[n], 0)) >= 0) {
            input_event(vin_ev[n][id]);
            vq_give(&vin[n], (unsigned)id, &vin_ev[n][id], sizeof vin_ev[n][id], 1);   /* hand the buffer back */
            vq_kick(&vin[n]);
        }
    }
}

/* Network: queue 0 receives, queue 1 sends. Every frame carries a 12-byte virtio-net header in front, all zero here
   (no checksum offload, no segmentation). The first proof is ARP: ask QEMU's user-mode router (10.0.2.2) for its
   hardware address and print the answer. */
#define NET_BUF 1536
static struct vq net_rx, net_tx;
static unsigned char *net_rxbuf, *net_txbuf, net_mac[6];
static int net_up;
static const unsigned char my_ip[4] = { 10, 0, 2, 15 }, gw_ip[4] = { 10, 0, 2, 2 };
static void uart_mac(const unsigned char *m) {
    for (int i = 0; i < 6; i++) { if (i) uart_putc(':'); uart_putc("0123456789abcdef"[m[i] >> 4]); uart_putc("0123456789abcdef"[m[i] & 15]); }
}
int virtio_net_init(void) {
    unsigned long b = vio_find(1, 0);
    if (!b || !vio_start(b, 1u << 5 /* VIRTIO_NET_F_MAC */)) return 0;
    if (!vq_setup(&net_rx, b, 0) || !vq_setup(&net_tx, b, 1)) return 0;
    net_rxbuf = kmalloc(NET_BUF * VQ); net_txbuf = kmalloc(NET_BUF);
    if (!net_rxbuf || !net_txbuf) { uart_puts("oom net\n"); return 0; }
    for (int i = 0; i < 6; i++) net_mac[i] = *(volatile unsigned char *)(b + 0x100 + i);   /* config space: mac first */
    for (unsigned i = 0; i < VQ; i++) vq_give(&net_rx, i, net_rxbuf + i * NET_BUF, NET_BUF, 1);
    VR(b, 0x70) = 1 | 2 | 8 | 4;                    /* driver ok */
    vq_kick(&net_rx);
    uart_puts("M2 net mac "); uart_mac(net_mac); uart_putc('\n');
    net_up = 1;
    return 1;
}
/* The card under the shared IP stack (drivers/nic.h; drivers/net.c sits on top). One send buffer, so a send waits
   until the device has handed back every frame it was lent, the ARP probe's included, before reusing it. */
int nic_init(void) { return net_up; }
void nic_mac(unsigned char mac[6]) { for (int i = 0; i < 6; i++) mac[i] = net_mac[i]; }
int nic_send(const void *frame, unsigned int len) {
    if (!net_up || len > NET_BUF - 12) return 0;
    for (int i = 0; i < 12; i++) net_txbuf[i] = 0;
    for (unsigned i = 0; i < len; i++) net_txbuf[12 + i] = ((const unsigned char *)frame)[i];
    vq_give(&net_tx, 0, net_txbuf, 12 + len, 0);
    vq_kick(&net_tx);
    unsigned long freq, t0, t; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(freq), "=r"(t0));
    while (net_tx.u->idx != net_tx.a->idx) {
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t));
        if (t - t0 > freq) return 0;
    }
    net_tx.seen = net_tx.u->idx;
    return 1;
}
unsigned int nic_recv(void *buf, unsigned int max) {
    unsigned len; int id;
    if (!net_up || (id = vq_take(&net_rx, &len)) < 0) return 0;
    unsigned char *r = net_rxbuf + (unsigned)id * NET_BUF;
    if (len > NET_BUF) len = NET_BUF;
    len = len > 12 ? len - 12 : 0;
    if (len > max) len = max;
    for (unsigned i = 0; i < len; i++) ((unsigned char *)buf)[i] = r[12 + i];
    vq_give(&net_rx, (unsigned)id, r, NET_BUF, 1);
    vq_kick(&net_rx);
    return len;
}
void virtio_net_probe(void) {
    unsigned char *f = net_txbuf;
    for (int i = 0; i < 12; i++) f[i] = 0;          /* virtio-net header */
    unsigned char *e = f + 12;
    for (int i = 0; i < 6; i++) { e[i] = 0xFF; e[6 + i] = net_mac[i]; }
    e[12] = 0x08; e[13] = 0x06;                     /* ARP */
    const unsigned char hdr[8] = { 0, 1, 8, 0, 6, 4, 0, 1 };   /* Ethernet, IPv4, 6, 4, request */
    for (int i = 0; i < 8; i++) e[14 + i] = hdr[i];
    for (int i = 0; i < 6; i++) { e[22 + i] = net_mac[i]; e[32 + i] = 0; }
    for (int i = 0; i < 4; i++) { e[28 + i] = my_ip[i]; e[38 + i] = gw_ip[i]; }
    vq_give(&net_tx, 0, f, 12 + 42, 0);
    vq_kick(&net_tx);
    unsigned long freq, t0, t; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(freq), "=r"(t0));
    do {
        unsigned len; int id;
        while ((id = vq_take(&net_rx, &len)) >= 0) {
            unsigned char *r = net_rxbuf + (unsigned)id * NET_BUF + 12;
            int arp_reply = len >= 12 + 42 && r[12] == 0x08 && r[13] == 0x06 && r[20] == 0 && r[21] == 2;
            int from_gw = r[28] == gw_ip[0] && r[29] == gw_ip[1] && r[30] == gw_ip[2] && r[31] == gw_ip[3];
            vq_give(&net_rx, (unsigned)id, net_rxbuf + (unsigned)id * NET_BUF, NET_BUF, 1);
            vq_kick(&net_rx);
            if (arp_reply && from_gw) { uart_puts("M2 net gateway 10.0.2.2 is "); uart_mac(r + 22); uart_putc('\n'); return; }
        }
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t));
    } while (t - t0 < freq * 2);
    uart_puts("M2 net no ARP reply\n");
}

/* Disk: one request queue. A request is three descriptors chained: a 16-byte header (type, sector), the data buffer,
   and one status byte the device writes back. Polled, one request at a time. M3d: writes too (type 1, and the buffer is
   the device's to read), and emmc.c takes the disk as its card through vblk_*, so the FAT code runs on virt as on a Pi. */
struct blk_hdr { unsigned type, reserved; unsigned long sector; };
static struct vq blk_q;
static int blk_ok;
static unsigned long blk_sectors;
int virtio_blk_init(void) {
    unsigned long b = vio_find(2, 0);
    if (!b || !vio_start(b, 0) || !vq_setup(&blk_q, b, 0)) return 0;
    VR(b, 0x70) = 1 | 2 | 8 | 4;                    /* driver ok */
    blk_sectors = (unsigned long)VR(b, 0x100) | (unsigned long)VR(b, 0x104) << 32;   /* config space: capacity */
    uart_puts("M2 blk sectors "); uart_dec((unsigned)blk_sectors); uart_putc('\n');
    blk_ok = 1;
    return 1;
}
static int blk_rw(unsigned long sector, void *buf, int write) {   /* one 512-byte sector, 1 on success */
    static struct blk_hdr hdr __attribute__((aligned(16)));
    static volatile unsigned char status __attribute__((aligned(16)));
    if (!blk_ok) return 0;
    hdr.type = write ? 1 : 0; hdr.reserved = 0; hdr.sector = sector;   /* 0 = read, 1 = write */
    status = 0xFF;
    dcache_clean(&hdr, sizeof hdr); dcache_clean(buf, 512); dcache_clean((void *)&status, 1);   /* QEMU is coherent; a real DMA engine is not */
    blk_q.d[0] = (struct vq_desc){ (unsigned long)&hdr, sizeof hdr, 1 /* next */, 1 };
    blk_q.d[1] = (struct vq_desc){ (unsigned long)buf, 512, (unsigned short)(write ? 1 : 1 | 2) /* next; device writes on a read */, 2 };
    blk_q.d[2] = (struct vq_desc){ (unsigned long)&status, 1, 2, 0 };
    blk_q.a->ring[blk_q.a->idx % VQ] = 0;
    __asm__ volatile ("dsb sy" ::: "memory");
    blk_q.a->idx++;
    vq_kick(&blk_q);
    unsigned long freq, t0, t; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(freq), "=r"(t0));
    while (vq_take(&blk_q, 0) < 0) {
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t));
        if (t - t0 > freq * 2) return 0;
    }
    dcache_clean(buf, 512); dcache_clean((void *)&status, 1);   /* drop our stale copies so the reads below see what the device wrote */
    return status == 0;
}
static int blk_read(unsigned long sector, void *buf) { return blk_rw(sector, buf, 0); }
int vblk_ready(void) { return blk_ok; }
unsigned long vblk_sectors(void) { return blk_sectors; }
int vblk_rw(unsigned long sector, void *buf, int write) { return blk_rw(sector, buf, write); }
void virtio_blk_probe(void) {
    static unsigned char sec[512] __attribute__((aligned(64)));
    if (!blk_read(0, sec)) { uart_puts("M2 blk read FAIL\n"); return; }
    sec[511] = 0;   /* the test disk's first sector is text, ends in a newline */
    uart_puts("M2 blk sector0 "); for (int i = 0; i < 32 && sec[i] >= 32 && sec[i] < 127; i++) uart_putc((char)sec[i]); uart_putc('\n');
}
#else
int virtio_input_init(void) { return 0; }
void virtio_input_poll(void) {}
int virtio_net_init(void) { return 0; }
void virtio_net_probe(void) {}
/* The Pi's network card is its Wi-Fi chip (wifi.c): up once the handshake has installed the keys. */
int wifi_nic_up(void); void wifi_nic_mac(unsigned char mac[6]); int wifi_nic_send(const void *f, unsigned len); unsigned wifi_nic_recv(void *b, unsigned max);
int nic_init(void) { return wifi_nic_up(); }
void nic_mac(unsigned char mac[6]) { wifi_nic_mac(mac); }
int nic_send(const void *frame, unsigned int len) { return wifi_nic_send(frame, len); }
unsigned int nic_recv(void *buf, unsigned int max) { return wifi_nic_recv(buf, max); }
int virtio_blk_init(void) { return 0; }
void virtio_blk_probe(void) {}
#endif

