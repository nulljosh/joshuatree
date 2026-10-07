/* M4 Wi-Fi, stage 1: the Pi 4's CYW43455 over SDIO. Ported in shape from Plan 9's ether4330.c (Richard Miller) and
   Circle's addon/wlan; iovar names from Linux brcmfmac. The chip sits on the Arasan SDHCI (EMMC1, 0xFE300000) with
   GPIO 34-39 in ALT3. Everything is polled from the main loop, every wait has a timeout, and every step prints one
   short `wifi ...` line so a photo of the monitor says how far a real board got. QEMU has no model of either the host
   or the chip, so there the whole thing ends at `wifi no host` and the desktop carries on. The pure-logic half (frame
   packing, escan parsing, NVRAM packing) lives in wifi_proto.h and has a host-side test. Only built for PI_BUILD. */
#ifdef PI_BUILD
#include "wifi_proto.h"
#define R32(a) (*(volatile unsigned *)(unsigned long)(a))
void kputs(const char *s); void kdec(unsigned v); void kx(unsigned v);
static unsigned long now(void) { unsigned long t; __asm__ volatile ("isb\n mrs %0, cntpct_el0" : "=r"(t)); return t; }
static unsigned long ticks_per_ms(void) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return (f ? f : 54000000) / 1000; }
extern const unsigned char wifi_fw_bin[], wifi_fw_nvram[], wifi_fw_clm[];
extern const unsigned wifi_fw_bin_len, wifi_fw_nvram_len, wifi_fw_clm_len;
static void mdelay(unsigned ms) { unsigned long t0 = now(), n = ticks_per_ms() * ms; while (now() - t0 < n) {} }
static unsigned c53_stage, c53_int, c53_state;   /* where the last CMD53 gave up and what the host said */
/* Plain-English progress: the step reached so far, out of the ten it takes to reach the internet. */
static unsigned wstep; static const char *wname[] = { "", "power", "chip answers", "bus up", "chip clock", "chip halted",
    "firmware upload", "firmware running", "scan", "join", "internet" };
static void step(unsigned n) { wstep = n; }
static void summary(void) { kputs("Wi-Fi: "); kdec(wstep); kputs(" of 10 steps done, stuck at "); kputs(wname[wstep < 10 ? wstep + 1 : 10]); kputs("\n"); }
static void fail(const char *step) { summary(); kputs("wifi FAIL "); kputs(step); if (c53_stage) { kputs(" stage "); kdec(c53_stage); kputs(" int "); kx(c53_int); kputs(" state "); kx(c53_state); } kputs("\n"); }

/* ---- SDHCI host (Arasan, EMMC1). Register names per the SD Host Controller spec; OSDev's SDHCI page. ---- */
#define GPIO 0xFE200000UL
#define SDH 0xFE300000UL
#define ARG 0x08
#define BLK 0x04
#define CMD 0x0C
#define RESP 0x10
#define DATA 0x20
#define STATE 0x24
#define CTL1 0x2C
#define INT 0x30
#define INTMASK 0x34
#define INTEN 0x38
static unsigned last_err;   /* the INT bits seen when the last wait ended in an error */
static int wait_int(unsigned mask, unsigned ms) {   /* 1 when one of `mask` fired without an error bit */
    unsigned long t0 = now(), n = ticks_per_ms() * ms;
    while (now() - t0 < n) { unsigned s = R32(SDH + INT); if (s & 0x8000) { last_err = s; R32(SDH + INT) = s; return 0; } if (s & mask) { R32(SDH + INT) = s & mask; return 1; } }
    return 0;
}
static int sd_cmd(unsigned idx, unsigned arg, unsigned rtype, unsigned *resp) {   /* rtype: 0 none, 2 R1/R5/R6 (48 bit, CRC and index checked), 3 R4 (48 bit, no CRC and no index: CMD5's reply carries 0x3f and 0x7f there, so checking them is a command error) */
    for (unsigned n = 0; R32(SDH + STATE) & 3; n++) if (n > 1000000) return 0;
    R32(SDH + INT) = 0xffffffff;
    R32(SDH + ARG) = arg;
    R32(SDH + CMD) = idx << 24 | (rtype == 3 ? 0x020000 : rtype ? 0x1A0000 : 0);
    if (!wait_int(1, 100)) return 0;
    if (resp) *resp = R32(SDH + RESP);
    return 1;
}
static int sd_init(void) {
    for (unsigned g = 34; g <= 39; g++) {   /* GPFSELn: 3 bits per pin, ALT3 = 7 */
        unsigned long r = GPIO + (g / 10) * 4, sh = (g % 10) * 3;
        R32(r) = (R32(r) & ~(7u << sh)) | 7u << sh;
    }
    R32(SDH + CTL1) = R32(SDH + CTL1) | 0x07000000;   /* reset all: SRST bits 24-26 of CTL1 (the byte at 0x2F; a
                                                         32-bit store there is an alignment fault on device memory) */
    for (unsigned n = 0; R32(SDH + CTL1) & 0x07000000; n++) if (n > 1000000) return 0;
    R32(SDH + CTL1) = 1 | 0x80 << 8 | 0xE << 16;   /* internal clock on, divider for ~400 kHz identification, timeout max */
    for (unsigned n = 0; !(R32(SDH + CTL1) & 2); n++) if (n > 1000000) return 0;
    R32(SDH + CTL1) |= 4;                           /* SD clock on */
    R32(SDH + INTEN) = 0xffffffff; R32(SDH + INTMASK) = 0xffffffff;
    mdelay(2);
    return 1;
}
/* CMD52: one byte on function `fn` at `addr`; CMD53 moves whole blocks (stage 1 uses it for firmware and frames). */
static int cmd52(unsigned fn, unsigned addr, int write, unsigned v, unsigned *out) {
    unsigned r, a = (write ? 0x80000000u : 0) | fn << 28 | (addr & 0x1ffff) << 9 | (v & 0xff);
    if (!sd_cmd(52, a, 2, &r) || (r & 0xcb00)) return 0;
    if (out) *out = r & 0xff;
    return 1;
}
static int c53_fail(unsigned stage) { c53_stage = stage; c53_int = R32(SDH + INT) | last_err; c53_state = R32(SDH + STATE); return 0; }
static int cmd53(unsigned fn, unsigned addr, int write, unsigned char *buf, unsigned n) {   /* byte mode, up to 512 */
    unsigned r, a = (write ? 0x80000000u : 0) | fn << 28 | 1u << 26 | (addr & 0x1ffff) << 9 | (n & 0x1ff);
    R32(SDH + BLK) = 1u << 16 | n;   /* one block of n bytes (count in the top half; a count of 0 moves nothing) */
    mdelay(1);   /* the BCM2835 host wants a couple of SD clocks between accesses; a millisecond is far more than enough */
    for (unsigned k = 0; R32(SDH + STATE) & 3; k++) if (k > 1000000) return c53_fail(1);
    R32(SDH + INT) = 0xffffffff; R32(SDH + ARG) = a;
    R32(SDH + CMD) = 53u << 24 | 0x1A0000 | 0x200000 | 0x2 | (write ? 0 : 0x10);   /* data present (bit 21), block count on, read = 0x10; the first real-board run had 0x20 (multi-block) here instead of data present, so no data ever moved and `arm halt` failed */
    if (!wait_int(1, 100)) return c53_fail(2);
    r = R32(SDH + RESP); if (r & 0xcb00) { c53_fail(3); c53_int = r; return 0; }
    if (!wait_int(write ? 0x10 : 0x20, 100)) return c53_fail(4);   /* buffer ready fires once per block, not per word: the third real-board run moved 4 bytes and stalled on 64 */
    for (unsigned i = 0; i < n; i += 4) {
        if (write) R32(SDH + DATA) = buf[i] | buf[i + 1] << 8 | buf[i + 2] << 16 | (unsigned)buf[i + 3] << 24;
        else { unsigned w = R32(SDH + DATA); buf[i] = w; buf[i + 1] = w >> 8; buf[i + 2] = w >> 16; buf[i + 3] = w >> 24; }
    }
    return wait_int(2, 100) ? 1 : c53_fail(5);
}

/* ---- Backplane: the chip's cores through function 1 (SBSDIO window registers 0x1000a-c pick a 32 KiB window). ---- */
#define SB_WIN 0x1000a
static unsigned win;
static int bp_window(unsigned addr) {
    unsigned w = addr & ~0x7fffu;
    if (w == win) return 1;
    if (!cmd52(1, SB_WIN, 1, w >> 8, 0) || !cmd52(1, SB_WIN + 1, 1, w >> 16, 0) || !cmd52(1, SB_WIN + 2, 1, w >> 24, 0)) return 0;
    win = w; return 1;
}
static unsigned bp_done;   /* bytes the last bp_write moved, printed when the firmware load fails */
static int bp_write(unsigned addr, const unsigned char *p, unsigned n) {
    bp_done = 0;
    while (n) {
        unsigned k = n > 64 ? 64 : n;
        if (!bp_window(addr) || !cmd53(1, 0x8000 | (addr & 0x7fff), 1, (unsigned char *)p, k)) return 0;
        addr += k; p += k; n -= k; bp_done += k;
    }
    return 1;
}
static int bp_write32(unsigned addr, unsigned v) { unsigned char b[4]; wr32(b, v); return bp_write(addr, b, 4); }
#define CHIP_RAM 0x198000       /* 43455: 1.5 MiB of SOCRAM at 0x198000; the ARM CR4 core at 0x18002000 */
#define CHIP_RAM_SIZE 0xc0000    /* brcmfmac chip.c for the 4345 family: 768 KiB; the old 0x120000 put the NVRAM past the end of RAM */
#define CR4_WRAP 0x18102000
static int fw_load(void) {
    if (!wifi_fw_bin_len) { kputs("wifi no firmware\n"); return 0; }
    static unsigned char nv[8192];   /* the NVRAM text packed as the firmware wants it (key=value strings, a length trailer) */
    unsigned nvsz = nvram_pack((const char *)wifi_fw_nvram, wifi_fw_nvram_len, nv, sizeof nv), nvat = CHIP_RAM + CHIP_RAM_SIZE - nvsz;
    if (!nvsz) { fail("nvram pack"); return 0; }
    if (fw_padded(wifi_fw_bin_len) > CHIP_RAM_SIZE - nvsz) { fail("fw size"); return 0; }
    /* Halt the ARM but take it OUT of reset, as brcmfmac's cr4_set_passive does: its TCM is the RAM we load, and a
       core held in reset stops answering (the seventh real-board run: 64 bytes in, then an R5 error, flags 0x1800).
       IOCTRL = CPUHALT|FGC|CLK, RESETCTRL 1 then 0, then IOCTRL = CPUHALT|CLK. */
    if (!bp_write32(CR4_WRAP + 0x408, 0x23) || !bp_write32(CR4_WRAP + 0x800, 1)) { fail("arm halt"); return 0; }
    mdelay(1);
    if (!bp_write32(CR4_WRAP + 0x800, 0) || !bp_write32(CR4_WRAP + 0x408, 0x21)) { fail("arm unreset"); return 0; }
    kputs("wifi arm halted\n"); step(5);
    if (!bp_write(CHIP_RAM, wifi_fw_bin, wifi_fw_bin_len)) { summary(); kputs("wifi FAIL fw load at byte "); kdec(bp_done); kputs(" stage "); kdec(c53_stage); kputs(" int "); kx(c53_int); kputs(" state "); kx(c53_state); kputs("\n"); return 0; }
    if (!bp_write(nvat, nv, nvsz)) { fail("nvram"); return 0; }
    kputs("wifi fw "); kdec(wifi_fw_bin_len / 1024); kputs("k loaded\n"); step(6);
    /* Start it the way brcmfmac's cr4_set_active does: the firmware's first word is the reset vector and goes to
       backplane address 0; the 802.11 core gets a reset with its PHY clock on (wrapper 0x18101000, best effort); then
       the ARM core is cycled through reset with CPUHALT dropped: IOCTRL=CPUHALT|FGC|CLK, RESETCTRL=1, IOCTRL=FGC|CLK,
       RESETCTRL=0, IOCTRL=CLK. The eighth real-board run loaded everything and the firmware never came up: the old
       code wrote the vector to a made-up register and left the NVRAM unpacked and past the end of RAM. */
    unsigned rstvec = wifi_fw_bin[0] | wifi_fw_bin[1] << 8 | wifi_fw_bin[2] << 16 | (unsigned)wifi_fw_bin[3] << 24;
    if (!bp_write32(0, rstvec)) { fail("reset vector"); return 0; }
    bp_write32(0x18101000 + 0x408, 0xf); bp_write32(0x18101000 + 0x800, 1); mdelay(1); bp_write32(0x18101000 + 0x408, 0x7);
    bp_write32(0x18101000 + 0x800, 0); mdelay(1); bp_write32(0x18101000 + 0x408, 0x5);
    if (!bp_write32(CR4_WRAP + 0x408, 0x23) || !bp_write32(CR4_WRAP + 0x800, 1)) { fail("arm run"); return 0; }
    mdelay(1);
    if (!bp_write32(CR4_WRAP + 0x408, 0x3) || !bp_write32(CR4_WRAP + 0x800, 0)) { fail("arm run"); return 0; }
    mdelay(1);
    if (!bp_write32(CR4_WRAP + 0x408, 0x1)) { fail("arm run"); return 0; }
    kputs("wifi arm running\n");
    mdelay(50);
    cmd52(0, 0x02, 1, 0x06, 0);   /* F2 enable again now that the firmware owns it, as brcmfmac enables F2 only after download */
    cmd52(1, 0x1000e, 1, 0x10, 0);   /* the firmware wants the high-throughput clock: HT_AVAIL_REQ 0x10 (it answers with HT_AVAIL 0x80) */
    for (unsigned n = 0;; n++) {   /* F2 ready (IORDY bit 2) says the firmware is up */
        unsigned v; if (cmd52(0, 0x03, 0, 0, &v) && (v & 4)) break;
        if (n > 500) { fail("fw ready"); return 0; }
        mdelay(10);
    }
    kputs("wifi fw ready\n"); step(7);
    return 1;
}

/* ---- Control path: BCDC over SDPCM on function 2. One request in flight, polled reply. ---- */
static unsigned char frame[2048] __attribute__((aligned(64)));
static unsigned seq, reqid;
static int iovar(const char *name, int set, void *buf, unsigned len, unsigned *status) {
    unsigned char p[1024]; unsigned k = 0;
    while (name[k]) { p[k] = name[k]; k++; } p[k++] = 0;
    for (unsigned i = 0; i < len && k < sizeof p; i++) p[k++] = ((unsigned char *)buf)[i];
    unsigned id = ++reqid & 0xffff, n = sdpcm_pack(frame, seq++, SDPCM_CONTROL, set ? BCDC_SET_VAR : BCDC_GET_VAR, id, set, p, k);
    if (!cmd53(2, 0x8000, 1, frame, fw_padded(n))) return 0;
    for (unsigned t = 0; t < 200; t++) {
        unsigned off, l, ch; int c;
        if (!cmd53(2, 0x8000, 0, frame, 512)) return 0;
        c = sdpcm_parse(frame, 512, &off, &l);
        if (c == SDPCM_CONTROL && bcdc_reply(frame + off, l, id, &ch) >= 0) {
            *status = rd32(frame + off + 12);
            unsigned have = l - ch; if (have > len) have = len;
            for (unsigned i = 0; i < have; i++) ((unsigned char *)buf)[i] = frame[off + ch + i];
            return 1;
        }
        mdelay(5);
    }
    return 0;
}
static void ap_line(int rssi, unsigned chan, const char *ssid, unsigned slen) {   /* "wifi ap -51 ch6 MySSID", under 53 columns */
    kputs("wifi ap "); if (rssi < 0) { kputs("-"); rssi = -rssi; } kdec((unsigned)rssi); kputs(" ch"); kdec(chan); kputs(" ");
    char s[33]; unsigned n = slen > 32 ? 32 : slen; for (unsigned i = 0; i < n; i++) s[i] = ssid[i] >= 32 && ssid[i] < 127 ? ssid[i] : '?'; s[n] = 0;
    kputs(s); kputs("\n");
}
static int scan(void) {
    unsigned st; unsigned char v[64];
    if (!iovar("ver", 0, v, sizeof v, &st) || st) { fail("ver"); return 0; }
    v[48] = 0; for (unsigned i = 0; i < 48; i++) if (v[i] == '\n') v[i] = 0;
    kputs("wifi ver "); kputs((char *)v); kputs("\n");
    unsigned char mac[6];
    if (!iovar("cur_etheraddr", 0, mac, 6, &st) || st) { fail("mac"); return 0; }
    kputs("wifi mac "); for (int i = 0; i < 6; i++) { kx(mac[i]); if (i < 5) kputs(":"); } kputs("\n");
    /* clmload: chunks of {flag, type, len, crc, data}; 1k at a time with BEGIN(2)/END(4) flags */
    for (unsigned off = 0; off < wifi_fw_clm_len; off += 1024) {
        unsigned k = wifi_fw_clm_len - off > 1024 ? 1024 : wifi_fw_clm_len - off, fl = (off == 0 ? 2 : 0) | (off + k >= wifi_fw_clm_len ? 4 : 0) | 1 << 12;
        unsigned char b[1040]; wr16(b, fl); wr16(b + 2, 2); wr32(b + 4, k); wr32(b + 8, 0);
        for (unsigned i = 0; i < k; i++) b[12 + i] = wifi_fw_clm[off + i];
        if (!iovar("clmload", 1, b, 12 + k, &st) || st) { fail("clm"); return 0; }
    }
    unsigned char cc[12] = { 'C', 'A', 0, 0, 0xff, 0xff, 0xff, 0xff, 'C', 'A', 0, 0 };   /* wlc_country: ccode, rev -1, abbrev */
    if (!iovar("country", 1, cc, 12, &st) || st) { fail("country"); return 0; }
    unsigned char es[80] = {0}; wr32(es, 1); wr16(es + 4, 1); wr16(es + 6, 0x1234);   /* escan: version 1, ESCAN_ACTION_START, sync id */
    unsigned char *pr = es + 8; for (int i = 0; i < 6; i++) pr[36 + i] = 0xff;        /* wl_scan_params: wildcard SSID, any BSSID */
    pr[42] = 0; pr[43] = 2; wr32(pr + 44, (unsigned)-1); wr32(pr + 48, (unsigned)-1); wr32(pr + 52, (unsigned)-1); wr32(pr + 56, 0);
    if (!iovar("escan", 1, es, sizeof es, &st) || st) { fail("escan"); return 0; }
    unsigned found = 0;
    for (unsigned t = 0; t < 600; t++) {   /* up to 3 s of events on the event channel */
        unsigned off, l; if (!cmd53(2, 0x8000, 0, frame, 1536)) break;
        if (sdpcm_parse(frame, 1536, &off, &l) == SDPCM_EVENT && l > 4 + 48 + 12) {
            const unsigned char *ev = frame + off + 4;   /* past the BCDC data header; brcmf_event: 14 eth + 10 bcmeth + 24 msg */
            unsigned type = ev[24 + 2] << 8 | ev[24 + 3];
            if (type == WLC_E_ESCAN_RESULT) found += escan_walk(ev + 48, l - 4 - 48, ap_line);
            unsigned stat = ev[24 + 4] << 24 | ev[24 + 5] << 16 | ev[24 + 6] << 8 | ev[24 + 7];
            if (type == WLC_E_ESCAN_RESULT && stat != 8) break;   /* anything but WLC_E_STATUS_PARTIAL ends the scan */
        }
        mdelay(5);
    }
    kputs("wifi scan done, "); kdec(found); kputs(" networks\n"); step(8); summary();
    return 1;
}

/* WLAN power. On the Pi 4 the chip's WL_ON line is pin 1 of the firmware's GPIO expander (expander pins are numbered
   from 128), reached only through the mailbox. Tags from Linux's include/soc/bcm2835/raspberrypi-firmware.h:
   0x00030043 get config, 0x00038043 set config, 0x00030041 get state, 0x00038041 set state. Following
   drivers/gpio/gpio-raspberrypi-exp.c: read the pin's polarity, make it an output (direction 1) driven high, then set
   the state high and read it back. The first real-board boot (2026-10-06) failed at CMD5 because the old code sent
   the two "get" tags, so the chip was never powered. The message lives in cacheable RAM, so it is cleaned out to RAM
   before the GPU reads it and invalidated before we read the reply, like the framebuffer mailbox in main.c. */
#define WL_ON 129
static volatile unsigned wmbox[16] __attribute__((aligned(64)));
static void wmbox_flush(void) {
    for (unsigned long a = (unsigned long)wmbox & ~63UL; a < (unsigned long)wmbox + sizeof wmbox; a += 64)
        __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
}
static int wmbox_call(void) {
    unsigned long m = 0xFE00B880UL, a = (unsigned long)wmbox | 8;
    wmbox_flush();
    for (unsigned n = 0; R32(m + 0x38) & 0x80000000u; n++) if (n > 1000000) return 0;
    R32(m + 0x20) = (unsigned)a;
    for (unsigned n = 0; ; n++) {
        if (n > 1000000) return 0;
        if (R32(m + 0x18) & 0x40000000u) continue;
        if (R32(m + 0x00) == (unsigned)a) { wmbox_flush(); return wmbox[1] == 0x80000000u; }
    }
}
static void wifi_power_on(void) {
    /* get config: gpio, direction, polarity, term_en, term_pull_up (24-byte value buffer) */
    wmbox[0] = 48; wmbox[1] = 0; wmbox[2] = 0x00030043; wmbox[3] = 24; wmbox[4] = 0;
    wmbox[5] = WL_ON; wmbox[6] = 0; wmbox[7] = 0; wmbox[8] = 0; wmbox[9] = 0; wmbox[10] = 0; wmbox[11] = 0;
    unsigned pol = wmbox_call() ? wmbox[7] : 0;
    /* set config: output, keep polarity, no termination, driven high */
    wmbox[0] = 48; wmbox[1] = 0; wmbox[2] = 0x00038043; wmbox[3] = 24; wmbox[4] = 0;
    wmbox[5] = WL_ON; wmbox[6] = 1; wmbox[7] = pol; wmbox[8] = 0; wmbox[9] = 0; wmbox[10] = 1; wmbox[11] = 0;
    int cfg = wmbox_call();
    /* set state high */
    wmbox[0] = 32; wmbox[1] = 0; wmbox[2] = 0x00038041; wmbox[3] = 8; wmbox[4] = 0; wmbox[5] = WL_ON; wmbox[6] = 1; wmbox[7] = 0;
    int set = wmbox_call();
    /* read it back */
    wmbox[0] = 32; wmbox[1] = 0; wmbox[2] = 0x00030041; wmbox[3] = 8; wmbox[4] = 0; wmbox[5] = WL_ON; wmbox[6] = 0; wmbox[7] = 0;
    int got = wmbox_call() ? (int)wmbox[6] : -1;
    if (!cfg && !set) kputs("wifi power on (no mailbox)\n");
    else if (got == 1) kputs("wifi power on, WL_ON reads 1\n"), step(1);
    else kputs(got == 0 ? "wifi power FAIL: WL_ON reads 0\n" : "wifi power on, readback failed\n");
    mdelay(150);
}

int wifi_init(void) {
    unsigned r;
    wifi_power_on();
    if (!sd_init()) { kputs("wifi no host\n"); return 0; }
    if (!sd_cmd(0, 0, 0, 0)) { kputs("wifi no host\n"); return 0; }
    if (!sd_cmd(5, 0, 3, &r)) { fail("cmd5"); return 0; }               /* IO_SEND_OP_COND: any SDIO card there? */
    if (!sd_cmd(5, r & 0xffffff, 3, &r) || !(r & 0x80000000u)) { fail("cmd5 ocr"); return 0; }
    if (!sd_cmd(3, 0, 2, &r)) { fail("cmd3"); return 0; }               /* relative address */
    unsigned rca = r & 0xffff0000u;
    if (!sd_cmd(7, rca, 2, &r)) { fail("cmd7"); return 0; }             /* select */
    kputs("wifi sdio card rca "); kx(rca >> 16); kputs("\n"); step(2);
    if (!cmd52(0, 0x07, 1, 0x02, 0)) { fail("4-bit"); return 0; }       /* CCCR bus width 4 */
    R32(SDH + CTL1) = (R32(SDH + CTL1) & ~0xff00u) | 1 | 4 | 0x8 << 8;  /* base/16: 25 MHz or less, default speed (the card's high-speed mode is never enabled) */
    R32(SDH + 0x28) = (R32(SDH + 0x28) & ~0xffu) | 2;                  /* 4-bit, default speed */
    if (!cmd52(0, 0x02, 1, 0x06, 0)) { fail("f1f2 enable"); return 0; }
    for (unsigned n = 0;; n++) { unsigned v; if (cmd52(0, 0x03, 0, 0, &v) && (v & 2)) break; if (n > 100) { fail("f1 ready"); return 0; } mdelay(10); }
    kputs("wifi f1 f2 up\n"); step(3);
    /* The backplane (the chip's RAM and cores) only answers while its ALP clock runs: ask for it through CHIPCLKCSR
       (F1 0x1000e: ALP_AVAIL_REQ 0x08) and wait for ALP_AVAIL (0x40), like brcmfmac and
       Plan 9's ether4330. The fourth real-board run wrote 64 bytes and then got a general error on the next block. */
    if (!cmd52(1, 0x1000e, 1, 0x08, 0)) { fail("alp req"); return 0; }   /* ALP_AVAIL_REQ alone, as brcmfmac's htclk does; the sixth run with 0x28 (plus FORCE_HW_CLKREQ_OFF) lost the backplane again */
    for (unsigned n = 0;; n++) { unsigned v; if (cmd52(1, 0x1000e, 0, 0, &v) && (v & 0x40)) break; if (n > 100) { fail("alp"); return 0; } mdelay(5); }
    kputs("wifi alp clock up\n"); step(4);
    if (!fw_load()) return 0;
    return scan();
}
#else
int wifi_init(void) { return 0; }
#endif
