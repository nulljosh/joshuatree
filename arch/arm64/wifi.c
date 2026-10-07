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
static void fail(const char *step) { kputs("wifi FAIL "); kputs(step); kputs("\n"); }

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
static int wait_int(unsigned mask, unsigned ms) {   /* 1 when one of `mask` fired without an error bit */
    unsigned long t0 = now(), n = ticks_per_ms() * ms;
    while (now() - t0 < n) { unsigned s = R32(SDH + INT); if (s & 0x8000) { R32(SDH + INT) = s; return 0; } if (s & mask) { R32(SDH + INT) = s & mask; return 1; } }
    return 0;
}
static int sd_cmd(unsigned idx, unsigned arg, unsigned rtype, unsigned *resp) {   /* rtype: 0 none, 2 R1/R4/R5/R6 (48 bit) */
    for (unsigned n = 0; R32(SDH + STATE) & 3; n++) if (n > 1000000) return 0;
    R32(SDH + INT) = 0xffffffff;
    R32(SDH + ARG) = arg;
    R32(SDH + CMD) = idx << 24 | (rtype ? 0x1A0000 : 0);
    if (!wait_int(1, 100)) return 0;
    if (resp) *resp = R32(SDH + RESP);
    return 1;
}
static int sd_init(void) {
    for (unsigned g = 34; g <= 39; g++) {   /* GPFSELn: 3 bits per pin, ALT3 = 7 */
        unsigned long r = GPIO + (g / 10) * 4, sh = (g % 10) * 3;
        R32(r) = (R32(r) & ~(7u << sh)) | 7u << sh;
    }
    R32(SDH + 0x2F) = 0; R32(SDH + CTL1) = R32(SDH + CTL1) | 0x07000000;   /* reset all */
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
static int cmd53(unsigned fn, unsigned addr, int write, unsigned char *buf, unsigned n) {   /* byte mode, up to 512 */
    unsigned r, a = (write ? 0x80000000u : 0) | fn << 28 | 1u << 26 | (addr & 0x1ffff) << 9 | (n & 0x1ff);
    R32(SDH + BLK) = n;
    for (unsigned k = 0; R32(SDH + STATE) & 3; k++) if (k > 1000000) return 0;
    R32(SDH + INT) = 0xffffffff; R32(SDH + ARG) = a;
    R32(SDH + CMD) = 53u << 24 | 0x1A0000 | 0x20 | (write ? 0 : 0x10);
    if (!wait_int(1, 100)) return 0;
    r = R32(SDH + RESP); if (r & 0xcb00) return 0;
    for (unsigned i = 0; i < n; i += 4) {
        if (!wait_int(write ? 0x10 : 0x20, 100)) return 0;
        if (write) R32(SDH + DATA) = buf[i] | buf[i + 1] << 8 | buf[i + 2] << 16 | (unsigned)buf[i + 3] << 24;
        else { unsigned w = R32(SDH + DATA); buf[i] = w; buf[i + 1] = w >> 8; buf[i + 2] = w >> 16; buf[i + 3] = w >> 24; }
    }
    return wait_int(2, 100);
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
static int bp_write(unsigned addr, const unsigned char *p, unsigned n) {
    while (n) {
        unsigned k = n > 64 ? 64 : n;
        if (!bp_window(addr) || !cmd53(1, 0x8000 | (addr & 0x7fff), 1, (unsigned char *)p, k)) return 0;
        addr += k; p += k; n -= k;
    }
    return 1;
}
static int bp_write32(unsigned addr, unsigned v) { unsigned char b[4]; wr32(b, v); return bp_write(addr, b, 4); }
#define CHIP_RAM 0x198000       /* 43455: 1.5 MiB of SOCRAM at 0x198000; the ARM CR4 core at 0x18002000 */
#define CHIP_RAM_SIZE 0x120000
#define CR4_WRAP 0x18102000
static int fw_load(void) {
    if (!wifi_fw_bin_len) { kputs("wifi no firmware\n"); return 0; }
    unsigned nvsz = fw_padded(wifi_fw_nvram_len), nvat = CHIP_RAM + CHIP_RAM_SIZE - nvsz;
    if (fw_padded(wifi_fw_bin_len) > CHIP_RAM_SIZE - nvsz) { fail("fw size"); return 0; }
    /* hold the ARM in reset (wrapper RESETCTRL=1, IOCTRL=CPUHALT|CLK) while RAM is written */
    if (!bp_write32(CR4_WRAP + 0x800, 1) || !bp_write32(CR4_WRAP + 0x408, 0x21)) { fail("arm halt"); return 0; }
    if (!bp_write(CHIP_RAM, wifi_fw_bin, wifi_fw_bin_len)) { fail("fw load"); return 0; }
    if (!bp_write(nvat, wifi_fw_nvram, wifi_fw_nvram_len)) { fail("nvram"); return 0; }
    kputs("wifi fw "); kdec(wifi_fw_bin_len / 1024); kputs("k loaded\n");
    /* reset vector = start of RAM, then release: RESETCTRL=0, IOCTRL=CLK */
    if (!bp_write32(0x18002000 + 0x120, CHIP_RAM) || !bp_write32(CR4_WRAP + 0x800, 0) || !bp_write32(CR4_WRAP + 0x408, 1)) { fail("arm run"); return 0; }
    for (unsigned n = 0;; n++) {   /* F2 ready (IORDY bit 2) says the firmware is up */
        unsigned v; if (cmd52(0, 0x03, 0, 0, &v) && (v & 4)) break;
        if (n > 300) { fail("fw ready"); return 0; }
        mdelay(10);
    }
    kputs("wifi fw ready\n");
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
    kputs("wifi scan done, "); kdec(found); kputs(" networks\n");
    return 1;
}

int wifi_init(void) {
    unsigned r;
    if (!sd_init()) { kputs("wifi no host\n"); return 0; }
    if (!sd_cmd(0, 0, 0, 0)) { kputs("wifi no host\n"); return 0; }
    if (!sd_cmd(5, 0, 2, &r)) { fail("cmd5"); return 0; }               /* IO_SEND_OP_COND: any SDIO card there? */
    if (!sd_cmd(5, r & 0xffffff, 2, &r) || !(r & 0x80000000u)) { fail("cmd5 ocr"); return 0; }
    if (!sd_cmd(3, 0, 2, &r)) { fail("cmd3"); return 0; }               /* relative address */
    unsigned rca = r & 0xffff0000u;
    if (!sd_cmd(7, rca, 2, &r)) { fail("cmd7"); return 0; }             /* select */
    kputs("wifi sdio card rca "); kx(rca >> 16); kputs("\n");
    if (!cmd52(0, 0x07, 1, 0x02, 0)) { fail("4-bit"); return 0; }       /* CCCR bus width 4 */
    R32(SDH + CTL1) = (R32(SDH + CTL1) & ~0xff00u) | 1 | 4 | 0x4 << 8;  /* ~50 MHz */
    R32(SDH + 0x28) = (R32(SDH + 0x28) & ~0xffu) | 2 | 4;              /* 4-bit, high speed */
    if (!cmd52(0, 0x02, 1, 0x06, 0)) { fail("f1f2 enable"); return 0; }
    for (unsigned n = 0;; n++) { unsigned v; if (cmd52(0, 0x03, 0, 0, &v) && (v & 2)) break; if (n > 100) { fail("f1 ready"); return 0; } mdelay(10); }
    kputs("wifi f1 f2 up\n");
    if (!fw_load()) return 0;
    return scan();
}
#else
int wifi_init(void) { return 0; }
#endif
