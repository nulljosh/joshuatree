/* M4: USB keyboards and mice through an xHCI controller, polled.

   The path on a real Pi 4: the BCM2711 PCIe root port, the VIA VL805 xHCI controller behind it, the VL805's own USB
   2.0 hub, then the keyboard or mouse (or a wireless receiver, which is a USB keyboard and mouse too). Under QEMU's
   virt machine the same code drives qemu-xhci, with a usb-hub in between. pci.c finds the controller.

   The shape, in order: reset the controller; give it the device context table (DCBAA), a command ring and one event
   ring segment; start it. Then for every connected root port: reset the port, Enable Slot, Address Device, read the
   device and configuration descriptors, Set Configuration. A hub gets its ports powered and reset and each device
   behind it is enumerated the same way with a route string (the hub port numbers, four bits a tier). A boot-protocol
   keyboard or mouse gets its interrupt IN endpoint configured, SET_PROTOCOL(boot), and one transfer always queued.
   usb_poll() drains the event ring: each completed report becomes Linux input events handed to main.c's input
   handling (kinput), then the transfer is queued again. No interrupts: the main loop calls usb_poll().

   Every structure the controller reads or writes lives in the heap, which is cacheable; the Pi's PCIe is not cache
   coherent, so every hand-over cleans or invalidates the lines (QEMU does not care either way). Every wait is bounded.

   Prior art: the xHCI 1.2 specification (sections 4.2 to 4.6, 6.2 to 6.4), the OSDev wiki's "eXtensible Host
   Controller Interface" page, Circle's lib/usb/xhci*.cpp for the order of operations, Linux's
   xhci_port_state_to_neutral for the PORTSC write mask and hid-input.c for the HID usage to key code table. */
#define R32(a) (*(volatile unsigned *)(unsigned long)(a))
void *kmalloc(unsigned int n);
void kputs(const char *s);
void kdec(unsigned v);
void kx(unsigned v);
void kinput(unsigned type, unsigned code, int value);   /* main.c: one Linux input event (EV_KEY, EV_REL, EV_SYN) */
unsigned long pci_xhci_base(void);

static unsigned long now(void) { unsigned long t; __asm__ volatile ("isb\n mrs %0, cntpct_el0" : "=r"(t)); return t; }
static unsigned long ticks_per_ms(void) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return (f ? f : 54000000) / 1000; }
static void mdelay(unsigned ms) { unsigned long t0 = now(), n = ticks_per_ms() * ms; while (now() - t0 < n) {} }
static void dma_sync(const volatile void *p, unsigned long n) {   /* clean and invalidate: hand lines to or back from the device */
    for (unsigned long a = (unsigned long)p & ~63UL; a < (unsigned long)p + n; a += 64) __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
}
static void *dma_alloc(unsigned n, unsigned align) {   /* zeroed, aligned, padded to whole cache lines; a 4 KiB alignment also keeps rings inside one 64 KiB boundary */
    n = (n + 63) & ~63u;
    unsigned long p = (unsigned long)kmalloc(n + align);
    if (!p) return 0;
    p = (p + align - 1) & ~((unsigned long)align - 1);
    for (unsigned i = 0; i < n; i++) ((volatile unsigned char *)p)[i] = 0;
    dma_sync((void *)p, n);
    return (void *)p;
}
static unsigned lo32(const volatile void *p) { return (unsigned)(unsigned long)p; }
static unsigned hi32(const volatile void *p) { return (unsigned)((unsigned long)p >> 32); }

/* ---- Registers ---- */
static unsigned long cap, op, rt, db;
static unsigned max_slots, max_ports, csz;
#define USBCMD  (op + 0x00)
#define USBSTS  (op + 0x04)
#define CRCR    (op + 0x18)
#define DCBAAP  (op + 0x30)
#define CONFIG  (op + 0x38)
#define PORTSC(n) (op + 0x400 + 0x10 * ((n) - 1))
#define IR0     (rt + 0x20)
/* PORTSC bits: the read-only and read-write ones that are safe to write back unchanged (Linux's
   xhci_port_state_to_neutral). Writing PED (bit 1) or any change bit (17 to 23) back as read would disable the port
   or acknowledge a change we have not looked at. */
#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR  (1u << 4)
#define PORT_PP  (1u << 9)
#define PORT_PRC (1u << 21)
#define PORT_KEEP (1u | (1u << 3) | (0xFu << 10) | (1u << 30) | (0xFu << 5) | (1u << 9) | (3u << 14) | (7u << 25))
#define PORT_CHANGES (0x7Fu << 17)

/* ---- Rings: TRBs are four 32-bit words. The last slot of every ring is a Link TRB back to the start, with the
   toggle-cycle flag, so the producer's cycle bit flips each lap. ---- */
#define RING_N 32
struct ring { volatile unsigned *t; unsigned idx, cycle; };
#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP 12
#define TRB_EVALUATE_CTX 13
#define EV_TRANSFER 32
#define EV_COMMAND 33
#define TRB_IOC (1u << 5)
#define TRB_ISP (1u << 2)
#define TRB_IDT (1u << 6)
static int ring_init(struct ring *r) {
    r->t = dma_alloc(RING_N * 16, 4096);
    if (!r->t) return 0;
    r->idx = 0; r->cycle = 1;
    volatile unsigned *l = r->t + (RING_N - 1) * 4;
    l[0] = lo32(r->t); l[1] = hi32(r->t); l[2] = 0; l[3] = (TRB_LINK << 10) | 2;   /* toggle cycle; its own cycle bit is set when we reach it */
    dma_sync(r->t, RING_N * 16);
    return 1;
}
static unsigned long ring_push(struct ring *r, unsigned p0, unsigned p1, unsigned st, unsigned ctl) {
    volatile unsigned *v = r->t + r->idx * 4;
    v[0] = p0; v[1] = p1; v[2] = st;
    __asm__ volatile ("dmb sy" ::: "memory");
    v[3] = (ctl & ~1u) | r->cycle;                 /* the cycle bit last: it hands the TRB over */
    dma_sync(v, 16);
    if (++r->idx == RING_N - 1) {                  /* reached the link: hand it over too, wrap, flip */
        volatile unsigned *l = r->t + (RING_N - 1) * 4;
        l[3] = (TRB_LINK << 10) | 2 | r->cycle;
        dma_sync(l, 16);
        r->idx = 0; r->cycle ^= 1;
    }
    return (unsigned long)v;
}

static struct ring cmd_ring;
static volatile unsigned *ev_ring;
static unsigned ev_idx, ev_cycle;
#define EV_N 64
static unsigned long *dcbaa;

/* ---- Devices ---- */
#define MAX_DEV 16
struct usbdev {
    unsigned slot, speed, route, root_port, depth;
    unsigned tt_slot, tt_port;                     /* the high-speed hub whose transaction translator a LS/FS device sits behind */
    unsigned is_hub, nports, ttt, mtt;
    unsigned mps0;
    struct ring ep0;
    void *in_ctx, *out_ctx;
    unsigned kind;                                 /* 1 keyboard, 2 mouse */
    unsigned dci, mps, iface, reqlen;
    struct ring intr;
    unsigned char *buf, last[8];
};
static struct usbdev devs[MAX_DEV];
static unsigned ndevs, nkbd, nmouse;

static volatile unsigned *ctx(void *base, unsigned i) { return (volatile unsigned *)((char *)base + i * csz); }

/* ---- Events ---- */
static int cmd_done, ctl_done;
static unsigned cmd_cc, cmd_slot, ctl_cc;
static void hid_report(struct usbdev *d, unsigned len);
static void queue_intr(struct usbdev *d);
static int handle_events(void) {   /* everything the controller has posted; returns how many */
    int n = 0;
    for (;;) {
        volatile unsigned *e = ev_ring + ev_idx * 4;
        dma_sync(e, 16);
        if ((e[3] & 1) != ev_cycle) break;
        unsigned p0 = e[0], st = e[2], c = e[3], type = (c >> 10) & 0x3F, slot = c >> 24;
        (void)p0;
        if (++ev_idx == EV_N) { ev_idx = 0; ev_cycle ^= 1; }
        n++;
        if (type == EV_COMMAND) { cmd_done = 1; cmd_cc = st >> 24; cmd_slot = slot; }
        else if (type == EV_TRANSFER) {
            unsigned dci = (c >> 16) & 0x1F, cc = st >> 24;
            for (unsigned i = 0; i < ndevs; i++) {
                struct usbdev *d = &devs[i];
                if (d->slot != slot) continue;
                if (dci == 1) { ctl_done = 1; ctl_cc = cc; }
                else if (d->kind && dci == d->dci) {
                    if (cc == 1 || cc == 13) hid_report(d, d->reqlen - (st & 0xFFFFFF));
                    if (cc == 1 || cc == 13) queue_intr(d);   /* anything else halted the endpoint: stop listening */
                    else { kputs("usb transfer error "); kdec(cc); kputs("\n"); }
                }
            }
        }   /* port status changes and the rest: the scan below reads PORTSC itself */
    }
    if (n) {
        unsigned long dq = (unsigned long)(ev_ring + ev_idx * 4);
        R32(IR0 + 0x18) = (unsigned)dq | 8;   /* ERDP, with EHB (write 1 to clear) */
        R32(IR0 + 0x1C) = (unsigned)(dq >> 32);
    }
    return n;
}
static int wait_flag(int *flag, unsigned ms) {
    unsigned long t0 = now(), lim = ticks_per_ms() * ms;
    while (!*flag) { handle_events(); if (now() - t0 > lim) return 0; }
    return 1;
}
static void doorbell(unsigned slot, unsigned target) { __asm__ volatile ("dsb sy" ::: "memory"); R32(db + 4 * slot) = target; }

static int command(unsigned p0, unsigned p1, unsigned ctl) {   /* 1 when it completed with Success */
    cmd_done = 0;
    ring_push(&cmd_ring, p0, p1, 0, ctl);
    doorbell(0, 0);
    if (!wait_flag(&cmd_done, 500)) { kputs("usb xhci command timeout\n"); return 0; }
    return cmd_cc == 1;
}

/* A control transfer on endpoint 0: Setup, optional Data, Status (opposite direction, or IN with no data). */
static int control(struct usbdev *d, unsigned rtype, unsigned req, unsigned val, unsigned idx, unsigned len, void *buf) {
    int in = (rtype & 0x80) != 0;
    if (len) dma_sync(buf, len);
    ctl_done = 0;
    ring_push(&d->ep0, rtype | req << 8 | val << 16, idx | len << 16, 8, (TRB_SETUP << 10) | TRB_IDT | ((len ? (in ? 3u : 2u) : 0u) << 16));
    if (len) ring_push(&d->ep0, lo32(buf), hi32(buf), len, (TRB_DATA << 10) | (in ? 1u << 16 : 0));
    ring_push(&d->ep0, 0, 0, 0, (TRB_STATUS << 10) | TRB_IOC | (len && in ? 0 : 1u << 16));
    doorbell(d->slot, 1);
    if (!wait_flag(&ctl_done, 500)) { kputs("usb control timeout\n"); return 0; }
    if (len) dma_sync(buf, len);
    return ctl_cc == 1 || ctl_cc == 13;
}

/* The slot context: where the device is (root port, route string, speed), whether it is a hub, and for a LS/FS
   device behind a high-speed hub, which hub and port hold its transaction translator. */
static void fill_slot(struct usbdev *d, volatile unsigned *s, unsigned entries) {
    s[0] = d->route | d->speed << 20 | d->mtt << 25 | d->is_hub << 26 | entries << 27;
    s[1] = d->root_port << 16 | d->nports << 24;
    s[2] = d->tt_slot | d->tt_port << 8 | d->ttt << 16;
    s[3] = 0;
}
static void fill_ep0(struct usbdev *d, volatile unsigned *e) {
    e[0] = 0;
    e[1] = 3u << 1 | 4u << 3 | d->mps0 << 16;          /* 3 retries, control endpoint */
    e[2] = lo32(d->ep0.t) | 1; e[3] = hi32(d->ep0.t);   /* dequeue pointer and cycle state */
    e[4] = 8;                                           /* average TRB length */
}
static void clear_in(struct usbdev *d) {
    for (unsigned i = 0; i < 33 * csz; i += 4) *(volatile unsigned *)((char *)d->in_ctx + i) = 0;
}

static void queue_intr(struct usbdev *d) {
    dma_sync(d->buf, 64);
    d->reqlen = d->mps > 64 ? 64 : d->mps;
    ring_push(&d->intr, lo32(d->buf), hi32(d->buf), d->reqlen, (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
    doorbell(d->slot, d->dci);
}

/* HID usage IDs (keyboard page) to Linux key codes, the first 128, from Linux's hid-input.c. 0 is no key. */
static const unsigned char hid_keys[128] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113 };
static const unsigned char mod_keys[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };   /* left ctrl shift alt meta, then right */
/* Echo a key press on the screen too, as its usage code and character: "usb key 0x0d j" */
static void echo_key(unsigned usage, int shift) {
    char c = 0;
    if (usage >= 4 && usage <= 29) c = (char)((shift ? 'A' : 'a') + usage - 4);
    else if (usage >= 30 && usage <= 38) c = (char)('1' + usage - 30);
    else if (usage == 39) c = '0';
    else if (usage == 44) c = '_';
    kputs("usb key 0x"); if (usage < 16) kputs("0"); kx(usage);
    if (c) { char s[3] = { ' ', c, 0 }; kputs(s); }
    kputs("\n");
}
static int has(const unsigned char *r, unsigned char k) { for (int i = 2; i < 8; i++) if (r[i] == k) return 1; return 0; }

static void hid_report(struct usbdev *d, unsigned len) {
    dma_sync(d->buf, 64);
    unsigned char *r = d->buf, *p = d->last;
    if (d->kind == 1) {   /* boot keyboard: modifiers, reserved, six keys down */
        if (len < 8 || r[2] == 1) return;             /* short, or "too many keys" (rollover error) */
        for (int b = 0; b < 8; b++)
            if ((r[0] ^ p[0]) & (1u << b)) kinput(1, mod_keys[b], (r[0] >> b) & 1);
        for (int i = 2; i < 8; i++) if (p[i] && !has(r, p[i]) && p[i] < 128 && hid_keys[p[i]]) kinput(1, hid_keys[p[i]], 0);
        for (int i = 2; i < 8; i++) if (r[i] && !has(p, r[i])) {
            echo_key(r[i], (r[0] & 0x22) != 0);
            if (r[i] < 128 && hid_keys[r[i]]) kinput(1, hid_keys[r[i]], 1);
        }
        kinput(0, 0, 0);
        for (int i = 0; i < 8; i++) p[i] = r[i];
    } else {              /* boot mouse: buttons, dx, dy (signed) */
        if (len < 3) return;
        for (int b = 0; b < 3; b++)
            if ((r[0] ^ p[0]) & (1u << b)) kinput(1, 272 + b, (r[0] >> b) & 1);   /* BTN_LEFT, BTN_RIGHT, BTN_MIDDLE */
        if (r[1]) kinput(2, 0, (signed char)r[1]);
        if (r[2]) kinput(2, 1, (signed char)r[2]);
        kinput(0, 0, 0);
        p[0] = r[0];
    }
}

static const char *speed_name(unsigned s) { return s == 1 ? "full" : s == 2 ? "low" : s == 3 ? "high" : s == 4 ? "super" : "?"; }
static void print_where(struct usbdev *d) {
    kdec(d->root_port);
    for (unsigned i = 0; i < d->depth; i++) { kputs("."); kdec((d->route >> (4 * i)) & 0xF); }
}

static void enumerate(unsigned root_port, unsigned route, unsigned depth, unsigned speed, struct usbdev *parent, unsigned parent_port);

/* Power, reset and enumerate every port of a USB 2.0 hub (hub class requests: 0xA0 and 0xA3 read, 0x23 writes). */
static void hub_init(struct usbdev *h, unsigned char *b) {
    if (!control(h, 0xA0, 6, 0x2900, 0, 8, b)) { kputs("usb hub descriptor FAIL\n"); return; }
    unsigned nports = b[2], chars = b[3] | b[4] << 8, pgood = b[5] * 2u;
    h->is_hub = 1; h->nports = nports;
    h->ttt = h->speed == 3 ? (chars >> 5) & 3 : 0;
    /* Tell the controller it is a hub: the Slot Context's Hub, port count and TT think time, through Configure Endpoint */
    clear_in(h);
    ctx(h->in_ctx, 0)[1] = 1;                                   /* add: slot context only */
    fill_slot(h, ctx(h->in_ctx, 1), 1);
    dma_sync(h->in_ctx, 33 * csz);
    if (!command(lo32(h->in_ctx), hi32(h->in_ctx), (TRB_CONFIGURE_EP << 10) | h->slot << 24)) kputs("usb hub slot update FAIL\n");
    kputs("usb hub port "); print_where(h); kputs(", "); kdec(nports); kputs(" ports\n");
    for (unsigned p = 1; p <= nports; p++) control(h, 0x23, 3, 8, p, 0, 0);   /* SET_FEATURE(PORT_POWER) */
    mdelay(pgood > 100 ? pgood : 100);
    for (unsigned p = 1; p <= nports && ndevs < MAX_DEV; p++) {
        if (!control(h, 0xA3, 0, 0, p, 4, b) || !(b[0] & 1)) continue;   /* GET_STATUS: nothing connected */
        control(h, 0x23, 3, 4, p, 0, 0);                                  /* SET_FEATURE(PORT_RESET) */
        int ok = 0;
        for (int t = 0; t < 50 && !ok; t++) {
            mdelay(10);
            if (control(h, 0xA3, 0, 0, p, 4, b) && (b[2] & 0x10) && !(b[0] & 0x10)) ok = 1;   /* C_PORT_RESET, reset over */
        }
        control(h, 0x23, 1, 20, p, 0, 0);                                 /* CLEAR_FEATURE(C_PORT_RESET) */
        control(h, 0x23, 1, 16, p, 0, 0);                                 /* CLEAR_FEATURE(C_PORT_CONNECTION) */
        if (!ok || !(b[0] & 2)) { kputs("usb hub port reset FAIL\n"); continue; }
        mdelay(10);                                                       /* reset recovery */
        unsigned spd = b[1] & 0x02 ? 2 : b[1] & 0x04 ? 3 : 1;            /* status bit 9 low speed, bit 10 high speed */
        enumerate(h->root_port, h->route | (p > 15 ? 15 : p) << (4 * h->depth), h->depth + 1, spd, h, p);
    }
}

static void enumerate(unsigned root_port, unsigned route, unsigned depth, unsigned speed, struct usbdev *parent, unsigned parent_port) {
    if (ndevs >= MAX_DEV || depth > 5) return;
    static unsigned char *b;
    if (!b) b = dma_alloc(256, 64);
    if (!command(0, 0, TRB_ENABLE_SLOT << 10)) { kputs("usb enable slot FAIL\n"); return; }
    struct usbdev *d = &devs[ndevs];
    *d = (struct usbdev){ 0 };
    d->slot = cmd_slot; d->speed = speed; d->route = route; d->root_port = root_port; d->depth = depth;
    if (parent && parent->speed == 3 && speed < 3) { d->tt_slot = parent->slot; d->tt_port = parent_port; }
    else if (parent) { d->tt_slot = parent->tt_slot; d->tt_port = parent->tt_port; }
    d->mps0 = speed == 4 ? 512 : speed == 3 ? 64 : 8;
    d->in_ctx = dma_alloc(33 * csz, 4096); d->out_ctx = dma_alloc(32 * csz, 4096);
    if (!d->in_ctx || !d->out_ctx || !ring_init(&d->ep0)) return;
    ndevs++;
    dcbaa[d->slot] = (unsigned long)d->out_ctx;
    dma_sync(&dcbaa[d->slot], 8);
    /* Address Device: slot and endpoint 0 */
    ctx(d->in_ctx, 0)[1] = 3;
    fill_slot(d, ctx(d->in_ctx, 1), 1);
    fill_ep0(d, ctx(d->in_ctx, 2));
    dma_sync(d->in_ctx, 33 * csz);
    if (!command(lo32(d->in_ctx), hi32(d->in_ctx), (TRB_ADDRESS_DEVICE << 10) | d->slot << 24)) {
        kputs("usb address device FAIL, code "); kdec(cmd_cc); kputs("\n"); return;
    }
    /* The first 8 bytes of the device descriptor carry endpoint 0's real packet size; tell the controller */
    if (!control(d, 0x80, 6, 0x0100, 0, 8, b)) { kputs("usb device descriptor FAIL\n"); return; }
    if (speed < 4 && b[7] && b[7] != d->mps0) {
        d->mps0 = b[7];
        clear_in(d);
        ctx(d->in_ctx, 0)[1] = 2;
        fill_ep0(d, ctx(d->in_ctx, 2));
        dma_sync(d->in_ctx, 33 * csz);
        if (!command(lo32(d->in_ctx), hi32(d->in_ctx), (TRB_EVALUATE_CTX << 10) | d->slot << 24)) kputs("usb evaluate context FAIL\n");
    }
    if (!control(d, 0x80, 6, 0x0100, 0, 18, b)) { kputs("usb device descriptor FAIL\n"); return; }
    unsigned dev_class = b[4], vid = b[8] | b[9] << 8, pid = b[10] | b[11] << 8;
    if (!control(d, 0x80, 6, 0x0200, 0, 9, b)) { kputs("usb config descriptor FAIL\n"); return; }
    unsigned total = b[2] | b[3] << 8;
    if (total > 256) total = 256;
    if (!control(d, 0x80, 6, 0x0200, 0, total, b)) { kputs("usb config descriptor FAIL\n"); return; }
    unsigned cfg_value = b[5], kind = 0, is_hub = dev_class == 9, iface = 0, ep_addr = 0, ep_mps = 0, ep_int = 0;
    unsigned cur_kind = 0, cur_iface = 0, in_hid = 0;
    for (unsigned i = 0; i + 2 <= total && b[i] >= 2 && i + b[i] <= total; i += b[i]) {
        if (b[i + 1] == 4) {   /* interface: class, subclass, protocol at 5, 6, 7 */
            in_hid = b[i + 5] == 3 && b[i + 6] == 1 && (b[i + 7] == 1 || b[i + 7] == 2) && !kind;
            cur_kind = b[i + 7]; cur_iface = b[i + 2];
            if (b[i + 5] == 9) is_hub = 1;
        } else if (b[i + 1] == 5 && in_hid && (b[i + 2] & 0x80) && (b[i + 3] & 3) == 3) {   /* interrupt IN endpoint */
            kind = cur_kind; iface = cur_iface; ep_addr = b[i + 2]; ep_mps = (b[i + 4] | b[i + 5] << 8) & 0x7FF; ep_int = b[i + 6];
            in_hid = 0;
        }
    }
    if (!control(d, 0x00, 9, cfg_value, 0, 0, 0)) { kputs("usb set configuration FAIL\n"); return; }
    kputs(is_hub ? "usb hub" : kind == 1 ? "usb kbd" : kind == 2 ? "usb mouse" : "usb other"); kputs(" addr "); kdec(d->slot);
    kputs(" port "); print_where(d); kputs(" "); kputs(speed_name(speed)); kputs(" "); kx(vid); kputs(":"); kx(pid); kputs("\n");
    if (is_hub) { hub_init(d, b); return; }
    if (!kind) return;
    /* Configure the interrupt IN endpoint: DCI = 2 * number + 1. The interval is in 125 us frames, as a power of two. */
    d->dci = 2 * (ep_addr & 0xF) + 1; d->mps = ep_mps ? ep_mps : 8; d->iface = iface;
    unsigned ival;
    if (speed >= 3) ival = ep_int ? ep_int - 1 : 0;
    else { unsigned f = (ep_int ? ep_int : 1) * 8; ival = 0; while ((2u << ival) <= f) ival++; if (ival < 3) ival = 3; if (ival > 10) ival = 10; }
    if (ival > 15) ival = 15;
    d->buf = dma_alloc(64, 64);
    if (!d->buf || !ring_init(&d->intr)) return;
    clear_in(d);
    ctx(d->in_ctx, 0)[1] = 1 | 1u << d->dci;
    fill_slot(d, ctx(d->in_ctx, 1), d->dci);
    volatile unsigned *e = ctx(d->in_ctx, d->dci + 1);
    e[0] = ival << 16;
    e[1] = 3u << 1 | 7u << 3 | d->mps << 16;            /* 3 retries, interrupt IN */
    e[2] = lo32(d->intr.t) | 1; e[3] = hi32(d->intr.t);
    e[4] = d->mps | d->mps << 16;                       /* average TRB length, max ESIT payload */
    dma_sync(d->in_ctx, 33 * csz);
    if (!command(lo32(d->in_ctx), hi32(d->in_ctx), (TRB_CONFIGURE_EP << 10) | d->slot << 24)) {
        kputs("usb configure endpoint FAIL, code "); kdec(cmd_cc); kputs("\n"); return;
    }
    control(d, 0x21, 0x0B, 0, iface, 0, 0);             /* SET_PROTOCOL(boot) */
    control(d, 0x21, 0x0A, 0, iface, 0, 0);             /* SET_IDLE(0): report only on change; a stall here is fine */
    d->kind = kind;
    if (kind == 1) nkbd++; else nmouse++;
    queue_intr(d);
}

int usb_init(void) {
    unsigned long base = pci_xhci_base();
    if (!base) return 0;
    cap = base;
    unsigned w0 = R32(cap);
    op = cap + (w0 & 0xFF);
    unsigned hcs1 = R32(cap + 4), hcs2 = R32(cap + 8), hcc1 = R32(cap + 0x10);
    rt = cap + (R32(cap + 0x18) & ~0x1Fu);
    db = cap + (R32(cap + 0x14) & ~0x3u);
    max_slots = hcs1 & 0xFF; max_ports = hcs1 >> 24; csz = hcc1 & 4 ? 64 : 32;
    if (max_slots > MAX_DEV) max_slots = MAX_DEV;
    kputs("usb xhci v"); kx(w0 >> 16); kputs(", "); kdec(max_ports); kputs(" ports, ctx "); kdec(csz); kputs("\n");
    /* BIOS hand-off, if a firmware claimed the controller (extended capability 1) */
    for (unsigned long x = hcc1 >> 16 ? cap + ((hcc1 >> 16) << 2) : 0; x; ) {
        unsigned v = R32(x);
        if ((v & 0xFF) == 1) {
            R32(x) = v | 1u << 24;
            for (int t = 0; t < 100 && (R32(x) & 1u << 16); t++) mdelay(10);
        }
        x = (v >> 8) & 0xFF ? x + (((v >> 8) & 0xFF) << 2) : 0;
    }
    /* Halt, reset, wait for Controller Not Ready to clear */
    unsigned long t0 = now(), lim = ticks_per_ms() * 1000;
    R32(USBCMD) = R32(USBCMD) & ~1u;
    while (!(R32(USBSTS) & 1)) if (now() - t0 > lim) { kputs("usb xhci will not halt\n"); return 0; }
    R32(USBCMD) = 2;
    while ((R32(USBCMD) & 2) || (R32(USBSTS) & 1u << 11)) if (now() - t0 > 2 * lim) { kputs("usb xhci reset timeout\n"); return 0; }
    R32(CONFIG) = max_slots;
    /* Device context table; entry 0 is the scratchpad array if the controller asks for one */
    dcbaa = dma_alloc(8 * 256, 4096);
    unsigned nscratch = ((hcs2 >> 27) & 0x1F) | ((hcs2 >> 21) & 0x1F) << 5;
    if (!dcbaa) return 0;
    if (nscratch) {
        unsigned long *arr = dma_alloc(8 * nscratch, 4096);
        if (!arr) return 0;
        for (unsigned i = 0; i < nscratch; i++) { void *pg = dma_alloc(4096, 4096); if (!pg) return 0; arr[i] = (unsigned long)pg; }
        dma_sync(arr, 8 * nscratch);
        dcbaa[0] = (unsigned long)arr;
        dma_sync(dcbaa, 8);
    }
    R32(DCBAAP) = lo32(dcbaa); R32(DCBAAP + 4) = hi32(dcbaa);
    if (!ring_init(&cmd_ring)) return 0;
    R32(CRCR) = lo32(cmd_ring.t) | 1; R32(CRCR + 4) = hi32(cmd_ring.t);   /* ring cycle state 1 */
    /* One event ring segment for interrupter 0: size, dequeue pointer, then the table address last */
    ev_ring = dma_alloc(EV_N * 16, 4096);
    volatile unsigned *erst = dma_alloc(16, 64);
    if (!ev_ring || !erst) return 0;
    ev_idx = 0; ev_cycle = 1;
    erst[0] = lo32(ev_ring); erst[1] = hi32(ev_ring); erst[2] = EV_N; erst[3] = 0;
    dma_sync(erst, 16);
    R32(IR0 + 0x08) = 1;
    R32(IR0 + 0x18) = lo32(ev_ring); R32(IR0 + 0x1C) = hi32(ev_ring);
    R32(IR0 + 0x10) = lo32(erst); R32(IR0 + 0x14) = hi32(erst);
    R32(IR0 + 0x00) = 0;                                  /* interrupter off: we poll */
    R32(USBCMD) = 1;                                      /* run */
    for (t0 = now(); R32(USBSTS) & 1; ) if (now() - t0 > lim) { kputs("usb xhci will not run\n"); return 0; }
    kputs("usb xhci run\n");
    /* Ports: power any that are off, give devices time to connect, then reset and enumerate each one with a device */
    int powered = 0;
    for (unsigned p = 1; p <= max_ports; p++)
        if (!(R32(PORTSC(p)) & PORT_PP)) { R32(PORTSC(p)) = (R32(PORTSC(p)) & PORT_KEEP) | PORT_PP; powered = 1; }
    mdelay(powered ? 300 : 50);
    handle_events();
    for (unsigned p = 1; p <= max_ports && ndevs < MAX_DEV; p++) {
        unsigned s = R32(PORTSC(p));
        if (!(s & PORT_CCS)) continue;
        kputs("usb port "); kdec(p); kputs(" connected\n");
        if (!(s & PORT_PED)) {                            /* USB 2 ports need a reset to become enabled; USB 3 ones train themselves */
            R32(PORTSC(p)) = (s & PORT_KEEP) | PORT_PR;
            for (t0 = now(); !(R32(PORTSC(p)) & PORT_PRC) && now() - t0 < lim / 2; ) {}
            mdelay(10);
        }
        s = R32(PORTSC(p));
        R32(PORTSC(p)) = (s & PORT_KEEP) | (s & PORT_CHANGES);   /* acknowledge what changed */
        if (!(s & PORT_PED)) { kputs("usb port "); kdec(p); kputs(" would not enable\n"); continue; }
        enumerate(p, 0, 0, (s >> 10) & 0xF, 0, 0);
    }
    kputs("usb ready: "); kdec(nkbd); kputs(" kbd, "); kdec(nmouse); kputs(" mouse\n");
    return (int)(nkbd + nmouse);
}

void usb_poll(void) { if (ev_ring) handle_events(); }
