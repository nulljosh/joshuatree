/* M4 Wi-Fi, stage 1: the Pi 4's CYW43455 over SDIO. Ported in shape from Plan 9's ether4330.c (Richard Miller) and
   Circle's addon/wlan; iovar names from Linux brcmfmac. The chip sits on the Arasan SDHCI (EMMC1, 0xFE300000) with
   GPIO 34-39 in ALT3. Everything is polled from the main loop, every wait has a timeout, and every step prints one
   short `wifi ...` line so a photo of the monitor says how far a real board got. QEMU has no model of either the host
   or the chip, so there the whole thing ends at `wifi no host` and the desktop carries on. The pure-logic half (frame
   packing, escan parsing, NVRAM packing) lives in wifi_proto.h and has a host-side test. Only built for PI_BUILD. */
#ifdef PI_BUILD
#include "wifi_cfg.h"
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
#define F2_BLOCK 512   /* F2 block size, set in CCCR FBR2 once F2 is enabled (brcmfmac uses 512 too) */
/* CMD53. Up to 512 bytes go in byte mode. Anything longer goes in block mode, F2_BLOCK bytes a block, and the
   buffer is padded up to whole blocks (SDPCM frames carry their own length, so the tail is ignored). The thirteenth
   real-board run failed at the CLM upload: a 1072-byte frame sent in byte mode, where the count field only holds
   nine bits, so the card expected 48 bytes and the host pushed 1072. Callers hand in buffers with room for the pad. */
static int cmd53(unsigned fn, unsigned addr, int write, unsigned char *buf, unsigned n) {
    unsigned blocks = 1, bytes = n, block = n;
    if (n > 512) { blocks = (n + F2_BLOCK - 1) / F2_BLOCK; block = F2_BLOCK; bytes = blocks * F2_BLOCK; }
    unsigned r, a = (write ? 0x80000000u : 0) | fn << 28 | (n > 512 ? 1u << 27 : 0) | 1u << 26 | (addr & 0x1ffff) << 9 | ((n > 512 ? blocks : n) & 0x1ff);
    R32(SDH + BLK) = blocks << 16 | block;
    for (unsigned k = 0; R32(SDH + STATE) & 3; k++) if (k > 1000000) return c53_fail(1);
    R32(SDH + INT) = 0xffffffff; R32(SDH + ARG) = a;
    R32(SDH + CMD) = 53u << 24 | 0x1A0000 | 0x200000 | 0x2 | (blocks > 1 ? 0x20 : 0) | (write ? 0 : 0x10);   /* data present, block count on, multi-block when more than one, read = 0x10 */
    if (!wait_int(1, 100)) return c53_fail(2);
    r = R32(SDH + RESP); if (r & 0xcb00) { c53_fail(3); c53_int = r; return 0; }
    for (unsigned i = 0; i < bytes; i += 4) {
        if ((i % block) == 0 && !wait_int(write ? 0x10 : 0x20, 100)) return c53_fail(4);   /* buffer ready fires once per block */
        if (write) { unsigned w = i < n ? buf[i] | buf[i + 1] << 8 | buf[i + 2] << 16 | (unsigned)buf[i + 3] << 24 : 0; R32(SDH + DATA) = w; }
        else { unsigned w = R32(SDH + DATA); if (i < n) { buf[i] = w; buf[i + 1] = w >> 8; buf[i + 2] = w >> 16; buf[i + 3] = w >> 24; } }
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
        unsigned k = n > 64 ? 64 : n;   /* function 1's byte-mode limit is its block size, 64: a 512-byte chunk (tried to speed the upload) fails on the first byte with OUT_OF_RANGE. The speed-up that mattered was dropping the 1 ms pause per access. */
        if (!bp_window(addr) || !cmd53(1, 0x8000 | (addr & 0x7fff), 1, (unsigned char *)p, k)) return 0;
        addr += k; p += k; n -= k; bp_done += k;
    }
    return 1;
}
static int bp_write32(unsigned addr, unsigned v) { unsigned char b[4]; wr32(b, v); return bp_write(addr, b, 4); }
static int bp_read(unsigned addr, unsigned char *p, unsigned n) {   /* read back through the same window, 64 bytes at a time */
    while (n) {
        unsigned k = n > 64 ? 64 : n;
        if (!bp_window(addr) || !cmd53(1, 0x8000 | (addr & 0x7fff), 0, p, k)) return 0;
        addr += k; p += k; n -= k;
    }
    return 1;
}
#define CHIP_RAM 0x198000       /* 43455: 1.5 MiB of SOCRAM at 0x198000; the ARM CR4 core at 0x18002000 */
#define CHIP_RAM_SIZE 0xc8000    /* brcmfmac chip.c, BRCM_CC_4345_CHIP_ID (the CYW43455 is a 4345 rev 6): 800 KiB, and the firmware looks for its NVRAM at the very end. 0xc0000 put it 32 KiB early and the firmware never raised HT (tenth real-board run: CLKCSR 0x50 = ALP up, HT requested, never granted). */
/* Where the cores are. The guesses below are brcmfmac's usual layout for the 4345 family; erom_walk() replaces them
   with what the chip's own enumeration ROM says (chipcommon 0x18000000, its EROM pointer at 0xfc), the way brcmfmac's
   chip.c does. A write to a wrong wrapper address succeeds on the bus and does nothing, which is one way the twelfth
   real-board run could look fine right up to the firmware never saying ready. */
static unsigned CR4_WRAP = 0x18102000, D11_WRAP = 0x18101000, SDIOD_CORE = 0x18003000;
static int bp_read32(unsigned addr, unsigned *v) { unsigned char b[4]; if (!bp_read(addr, b, 4)) return 0; *v = rd32(b); return 1; }
static void erom_walk(void) {
    unsigned id = 0, erom = 0;
    if (!bp_read32(0x18000000, &id) || !bp_read32(0x18000000 + 0xfc, &erom)) { kputs("wifi erom unreadable\n"); return; }
    kputs("wifi chip "); kx(id & 0xffff); kputs(" rev "); kdec((id >> 16) & 0xf); kputs(" erom "); kx(erom); kputs("\n");
    unsigned core = 0, regbase = 0, wrap = 0, found = 0;
    for (unsigned n = 0; n < 200; n++, erom += 4) {
        unsigned v; if (!bp_read32(erom, &v)) break;
        unsigned desc = v & 0xf;
        if (desc == 0xf) break;
        if (desc == 1) {   /* a component: two words, the first holds the core id */
            unsigned cib; erom += 4; if (!bp_read32(erom, &cib)) break;
            core = (v >> 8) & 0xfff; regbase = 0; wrap = 0; continue;
        }
        if (desc == 3) continue;   /* a master port */
        if (desc != 5) continue;   /* only address descriptors from here */
        unsigned stype = (v >> 6) & 3, sztype = (v >> 4) & 3;
        if (sztype == 3) { unsigned sz; erom += 4; if (!bp_read32(erom, &sz)) break; if (sz & 8) erom += 4; }
        if (v & 8) erom += 4;   /* a 64-bit address: skip the high word */
        unsigned base = v & 0xfffff000u;
        if (stype == 0 && !regbase) regbase = base;
        if ((stype == 2 || stype == 3) && !wrap) wrap = base;
        if (regbase && wrap) {
            if (core == 0x83e && !(found & 1)) { CR4_WRAP = wrap; found |= 1; }
            if (core == 0x812 && !(found & 2)) { D11_WRAP = wrap; found |= 2; }
            if (core == 0x829 && !(found & 4)) { SDIOD_CORE = regbase; found |= 4; }
            regbase = 0; wrap = 0; core = 0;   /* one pair per core is all we need */
        }
    }
    kputs("wifi cores cr4w "); kx(CR4_WRAP); kputs(" d11w "); kx(D11_WRAP); kputs(" sdiod "); kx(SDIOD_CORE); kputs(found == 7 ? "\n" : " (some guessed)\n");
}
static int fw_load(void) {
    if (!wifi_fw_bin_len) { kputs("wifi no firmware\n"); return 0; }
    static unsigned char nv[8192];   /* the NVRAM text packed as the firmware wants it (key=value strings, a length trailer) */
    unsigned nvsz = nvram_pack((const char *)wifi_fw_nvram, wifi_fw_nvram_len, nv, sizeof nv), nvat = CHIP_RAM + CHIP_RAM_SIZE - nvsz;
    if (!nvsz) { fail("nvram pack"); return 0; }
    if (fw_padded(wifi_fw_bin_len) > CHIP_RAM_SIZE - nvsz) { fail("fw size"); return 0; }
    /* Halt the ARM but take it OUT of reset, as brcmfmac's cr4_set_passive does: its TCM is the RAM we load, and a
       core held in reset stops answering (the seventh real-board run: 64 bytes in, then an R5 error, flags 0x1800).
       IOCTRL = CPUHALT|FGC|CLK, RESETCTRL 1 then 0, then IOCTRL = CPUHALT|CLK. */
    erom_walk();
    if (!bp_write32(CR4_WRAP + 0x408, 0x23) || !bp_write32(CR4_WRAP + 0x800, 1)) { fail("arm halt"); return 0; }
    mdelay(1);
    if (!bp_write32(CR4_WRAP + 0x800, 0) || !bp_write32(CR4_WRAP + 0x408, 0x21)) { fail("arm unreset"); return 0; }
    kputs("wifi arm halted\n"); step(5);
    if (!bp_write(CHIP_RAM, wifi_fw_bin, wifi_fw_bin_len)) { summary(); kputs("wifi FAIL fw load at byte "); kdec(bp_done); kputs(" stage "); kdec(c53_stage); kputs(" int "); kx(c53_int); kputs(" state "); kx(c53_state); kputs("\n"); return 0; }
    if (!bp_write(nvat, nv, nvsz)) { fail("nvram"); return 0; }
    kputs("wifi fw "); kdec(wifi_fw_bin_len / 1024); kputs("k loaded\n"); step(6);
    {   /* read the first and last 64 bytes back: a write that "succeeds" into the wrong place shows here */
        unsigned char chk[64]; unsigned bad = 0;
        if (!bp_read(CHIP_RAM, chk, 64)) { fail("fw verify read"); return 0; }
        for (unsigned i = 0; i < 64; i++) if (chk[i] != wifi_fw_bin[i]) { bad = i + 1; break; }
        if (!bad) { if (!bp_read(nvat + nvsz - 64, chk, 64)) { fail("nvram verify read"); return 0; }
                    for (unsigned i = 0; i < 64; i++) if (chk[i] != nv[nvsz - 64 + i]) { bad = 1000 + i + 1; break; } }
        if (bad) { summary(); kputs("wifi FAIL verify at "); kdec(bad - 1); kputs(bad > 1000 ? " (nvram)\n" : " (fw)\n"); return 0; }
        kputs("wifi fw verified\n");
    }
    /* Start it the way brcmfmac's cr4_set_active does: the firmware's first word is the reset vector and goes to
       backplane address 0; the 802.11 core gets a reset with its PHY clock on (wrapper 0x18101000, best effort); then
       the ARM core is cycled through reset with CPUHALT dropped: IOCTRL=CPUHALT|FGC|CLK, RESETCTRL=1, IOCTRL=FGC|CLK,
       RESETCTRL=0, IOCTRL=CLK. The eighth real-board run loaded everything and the firmware never came up: the old
       code wrote the vector to a made-up register and left the NVRAM unpacked and past the end of RAM. */
    unsigned rstvec = wifi_fw_bin[0] | wifi_fw_bin[1] << 8 | wifi_fw_bin[2] << 16 | (unsigned)wifi_fw_bin[3] << 24;
    if (!bp_write32(0, rstvec)) { fail("reset vector"); return 0; }
    bp_write32(D11_WRAP + 0x408, 0xf); bp_write32(D11_WRAP + 0x800, 1); mdelay(1); bp_write32(D11_WRAP + 0x408, 0x7);
    bp_write32(D11_WRAP + 0x800, 0); mdelay(1); bp_write32(D11_WRAP + 0x408, 0x5);
    if (!bp_write32(CR4_WRAP + 0x408, 0x23) || !bp_write32(CR4_WRAP + 0x800, 1)) { fail("arm run"); return 0; }
    mdelay(1);
    if (!bp_write32(CR4_WRAP + 0x408, 0x3) || !bp_write32(CR4_WRAP + 0x800, 0)) { fail("arm run"); return 0; }
    mdelay(1);
    if (!bp_write32(CR4_WRAP + 0x408, 0x1)) { fail("arm run"); return 0; }
    kputs("wifi arm running\n");
    mdelay(50);
    /* brcmfmac's firmware_callback, in order: ask for the HT clock and wait for HT_AVAIL, force HT on so the F2
       interrupt propagates, tell the firmware the SDPCM protocol version through the SDIO core's mailbox data
       register, enable F2, then wait for the card to say F2 is ready. */
    /* A bus trace for the photo: CLKCSR read after each step (0x1ff = the read itself failed, so the bus is gone). */
    unsigned t[4] = { 0x1ff, 0x1ff, 0x1ff, 0x1ff }, clk = 0;
    cmd52(1, 0x1000e, 0, 0, &t[0]);
    cmd52(1, 0x1000e, 1, 0x10, 0);
    { unsigned long t0 = now(), lim = ticks_per_ms() * 3000;
      while (now() - t0 < lim) { if (cmd52(1, 0x1000e, 0, 0, &clk) && (clk & 0x80)) break; mdelay(10); } }
    if (!(clk & 0x80)) { summary(); kputs("wifi FAIL ht clock: clkcsr "); kx(clk); kputs(" (the firmware did not start its clock)\n"); return 0; }
    kputs("wifi ht clock up\n");
    cmd52(1, 0x1000e, 1, clk | 0x02, 0); cmd52(1, 0x1000e, 0, 0, &t[1]);
    bp_write32(SDIOD_CORE + 0x48, 4u << 16);   /* tosbmailboxdata: SDPCM_PROT_VERSION 4 */
    cmd52(1, 0x1000e, 0, 0, &t[2]);
    cmd52(0, 0x02, 1, 0x06, 0);   /* F2 enable, now that the firmware owns it */
    cmd52(0, 0x210, 1, F2_BLOCK & 0xff, 0); cmd52(0, 0x211, 1, F2_BLOCK >> 8, 0);   /* FBR2 block size for the block-mode transfers */
    cmd52(1, 0x1000e, 0, 0, &t[3]);
    kputs("wifi bus "); kx(t[0]); kputs(" "); kx(t[1]); kputs(" "); kx(t[2]); kputs(" "); kx(t[3]); kputs("\n");
    { unsigned long t0 = now(), lim = ticks_per_ms() * 5000; unsigned v = 0, dead = 0;   /* F2 ready (IORDY bit 2) says the firmware is up */
      for (;;) {
        if (cmd52(0, 0x03, 0, 0, &v)) { dead = 0; if (v & 4) break; } else if (++dead > 10) { summary(); kputs("wifi FAIL bus dead after the firmware started\n"); return 0; }
        if (now() - t0 > lim) { unsigned ioe = 0, c2 = 0; cmd52(0, 0x02, 0, 0, &ioe); cmd52(1, 0x1000e, 0, 0, &c2);
                       /* What the firmware itself says: brcmfmac's readshared. It stores the address of its status block
                          in the last word of RAM once it is up; an assert or a trap sets flags 1 or 2. */
                       { unsigned char b[4], sh[32]; unsigned ptr = 0;
                         if (bp_read(CHIP_RAM + CHIP_RAM_SIZE - 4, b, 4)) ptr = rd32(b);
                         kputs("wifi shared ptr "); kx(ptr); kputs("\n");
                         if (ptr >= CHIP_RAM && ptr < CHIP_RAM + CHIP_RAM_SIZE - 32 && bp_read(ptr, sh, 32)) {
                           kputs("wifi fw flags "); kx(rd32(sh)); kputs(" trap "); kx(rd32(sh + 4)); kputs(" assert "); kx(rd32(sh + 8)); kputs(" line "); kdec(rd32(sh + 16)); kputs("\n"); } }
                       summary(); kputs("wifi FAIL fw ready e"); kx(ioe); kputs(" r"); kx(v); kputs(" c"); kx(c2); kputs("\n"); return 0; }
        mdelay(10);
      }
    }
    kputs("wifi fw ready\n"); step(7);
    return 1;
}

/* ---- Control path: BCDC over SDPCM on function 2. One request in flight, polled reply. ---- */
static unsigned char frame[2048] __attribute__((aligned(64)));
static unsigned seq, reqid;
/* Read ONE frame from F2: 64 bytes first, then only the rest the SDPCM length asks for, like brcmfmac's first-read.
   Reading a flat 1536 bytes swallowed the start of whatever frame came next, so once the firmware had events queued
   (after the radio came up) the replies to set-commands vanished: the seventeenth run saw 'status 0' no-replies for
   clmload, country and escan while the plain gets before them worked. */
static int f2_read(void) {
    if (!cmd53(2, 0x8000, 0, frame, 64)) return 0;
    unsigned len = frame[0] | frame[1] << 8;
    if (len > 64 && len <= 1600) { unsigned rest = (len - 64 + 3) & ~3u; if (!cmd53(2, 0x8000, 0, frame + 64, rest)) return 0; }
    return 1;
}
/* What the last frame read back looked like, printed when a command gets no matching reply. */
static unsigned last_chan = 98, last_len, last_cmd, last_id, last_st;
/* A control reply matches on the request id alone. The BCDC status is its own field: 0xffffffff (-1) is the chip saying "error",
   and bcdc_reply() in wifi_proto.h used -1 for "wrong id", so an error reply looked like no reply at all. */
static int reply_match(const unsigned char *b, unsigned len, unsigned id, unsigned *off) {
    if (len < 16 || (rd32(b + 8) >> 16) != id) return 0;
    *off = 16; return 1;
}
static unsigned tx_max, tx_max_seen;   /* the chip's flow control: the highest sequence number it will take, from byte 9 of every SDPCM header it sends */
/* brcmfmac only sends while seq != max and (max - seq) is not "negative" (bit 7). Frames sent past the credit are dropped by the chip,
   which looks exactly like a command that never gets a reply. Poll for fresh credit (any frame carries it) for up to half a second. */
static void wait_credit(void) {
    for (unsigned t = 0; t < 100 && tx_max_seen && (((tx_max - seq) & 0xff) == 0 || ((tx_max - seq) & 0x80)); t++) { f2_read(); mdelay(5); }
}
static void no_reply(const char *name, unsigned want) {
    kputs("wifi "); kputs(name); kputs(" no reply: want id "); kdec(want); kputs(", last frame ch "); kdec(last_chan);
    kputs(" len "); kdec(last_len); kputs(" cmd "); kdec(last_cmd); kputs(" id "); kdec(last_id); kputs(" st "); kx(last_st); kputs(" seq "); kdec(seq); kputs(" max "); kdec(tx_max); kputs("\n");
}
static int iovar(const char *name, int set, void *buf, unsigned len, unsigned *status) {
    unsigned char p[1536]; unsigned k = 0;   /* room for a 1 KiB CLM chunk, its 12-byte header and the name: a 1024-byte buffer here cut every chunk short, so the chip's whole-blob check failed (clmload_status 6) */
    while (name[k]) { p[k] = name[k]; k++; } p[k++] = 0;
    for (unsigned i = 0; i < len && k < sizeof p; i++) p[k++] = ((unsigned char *)buf)[i];
    wait_credit();
    unsigned id = ++reqid & 0xffff, n = sdpcm_pack(frame, seq++, SDPCM_CONTROL, set ? BCDC_SET_VAR : BCDC_GET_VAR, id, set, p, k);
    if (!cmd53(2, 0x8000, 1, frame, fw_padded(n))) return 0;
    for (unsigned t = 0; t < 200; t++) {
        unsigned off, l, ch; int c;
        if (!f2_read()) return 0;
        c = sdpcm_parse(frame, 1536, &off, &l);
        if (c >= 0) { tx_max = frame[9]; tx_max_seen = 1; last_chan = c; last_len = l; if (c == SDPCM_CONTROL && l >= 16) { last_cmd = rd32(frame + off); last_id = rd32(frame + off + 8) >> 16; last_st = rd32(frame + off + 12); } }
        else last_chan = 99;
        if (c == SDPCM_CONTROL && reply_match(frame + off, l, id, &ch)) {
            *status = rd32(frame + off + 12);
            unsigned have = l - ch; if (have > len) have = len;
            for (unsigned i = 0; i < have; i++) ((unsigned char *)buf)[i] = frame[off + ch + i];
            return 1;
        }
        mdelay(5);
    }
    no_reply(name, id);
    return 0;
}
/* A plain BCDC ioctl (no variable name): WLC_UP is 2, a set with no payload. brcmfmac brings the interface up this way
   before it scans; without it the chip refuses the scan. */
static int wlc_ioctl(unsigned cmd, void *buf, unsigned len, unsigned *status) {
    wait_credit();
    unsigned id = ++reqid & 0xffff, n = sdpcm_pack(frame, seq++, SDPCM_CONTROL, cmd, id, 1, buf, len);
    if (!cmd53(2, 0x8000, 1, frame, fw_padded(n))) return 0;
    for (unsigned t = 0; t < 200; t++) {
        unsigned off, l, ch; int c;
        if (!f2_read()) return 0;
        c = sdpcm_parse(frame, 1536, &off, &l);
        if (c == SDPCM_CONTROL && reply_match(frame + off, l, id, &ch)) { *status = rd32(frame + off + 12); return 1; }
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
        if (!iovar("clmload", 1, b, 12 + k, &st) || st) {   /* optional: the firmware carries a default CLM, brcmfmac only warns */
            kputs("wifi clm chunk at "); kdec(off); kputs(" of "); kdec(wifi_fw_clm_len); kputs(" refused, status "); kx(st); kputs("\n");
            { unsigned cs = 0xdead; unsigned s2; if (iovar("clmload_status", 0, &cs, 4, &s2)) { kputs("wifi clmload_status "); kx(cs); kputs("\n"); } }
            break; }
    }
    unsigned char cc[12] = { 'C', 'A', 0, 0, 0xff, 0xff, 0xff, 0xff, 'C', 'A', 0, 0 };   /* wlc_country: ccode, rev -1, abbrev */
    if (!iovar("country", 1, cc, 12, &st) || st) { kputs("wifi country refused, status "); kx(st); kputs(" (scanning with the default)\n"); }
    unsigned char es[80] = {0}; wr32(es, 1); wr16(es + 4, 1); wr16(es + 6, 0x1234);   /* escan: version 1, ESCAN_ACTION_START, sync id */
    unsigned char *pr = es + 8; for (int i = 0; i < 6; i++) pr[36 + i] = 0xff;        /* wl_scan_params: wildcard SSID, any BSSID */
    pr[42] = 2; pr[43] = 0;   /* bss_type ANY (2), scan_type active (0): the two bytes were swapped, so the chip refused the scan */
    wr32(pr + 44, (unsigned)-1); wr32(pr + 48, (unsigned)-1); wr32(pr + 52, (unsigned)-1); wr32(pr + 56, (unsigned)-1);   /* nprobes, active, passive, home time: -1 = the chip's defaults */
    /* Scan results come back as events, and the chip only sends the ones in its event mask: turn on ESCAN_RESULT (event 69,
       so byte 8, bit 5) the way brcmfmac does, reading the current 18-byte mask first. */
    { unsigned char em[20] = {0}; if (iovar("event_msgs", 0, em, 18, &st) && !st) { em[8] |= 1 << 5; em[0] |= 1 | 1 << 3 | 1 << 7; em[2] |= 1; em[5] |= 1 << 6;   /* + SET_SSID 0, AUTH 3, ASSOC 7, LINK 16, PSK_SUP 46 */ if (!iovar("event_msgs", 1, em, 18, &st) || st) kputs("wifi event mask not set\n"); } else kputs("wifi event mask unreadable\n"); }
    if (!wlc_ioctl(2, 0, 0, &st) || st) { kputs("wifi up ioctl status "); kdec(st); kputs("\n"); } else kputs("wifi radio up\n");   /* WLC_UP */
    { unsigned ok = iovar("escan", 1, es, sizeof es, &st);
      if (!ok || st) { kputs("wifi escan "); kputs(ok ? "status " : "no reply, status "); kx(st); kputs("\n"); fail("escan"); return 0; } }
    unsigned found = 0;
    /* brcmf_event, big-endian: 14 bytes of Ethernet header, 10 of bcmeth (subtype, length, version, oui, usr_subtype), then
       the 48-byte event message: version 2, flags 2, event_type 4, status 4, reason 4, auth_type 4, datalen 4, addr 6,
       ifname 16, ifidx 1, bsscfgidx 1. The data follows at 72. The first real scan read the flags as the type and so
       never saw a single result (scan done, 0 networks). */
    unsigned seen = 0;
    for (unsigned t = 0; t < 1600; t++) {   /* up to 8 s: an active scan of both bands takes a few seconds */
        unsigned off, l; if (!f2_read()) break;
        if (sdpcm_parse(frame, 1536, &off, &l) == SDPCM_EVENT && l > 4 + 72 + 12) {
            const unsigned char *ev = frame + off + 4;   /* past the BCDC data header */
            unsigned type = (unsigned)ev[28] << 24 | ev[29] << 16 | ev[30] << 8 | ev[31];
            unsigned stat = (unsigned)ev[32] << 24 | ev[33] << 16 | ev[34] << 8 | ev[35];
            if (seen < 6) { kputs("wifi event "); kdec(type); kputs(" status "); kdec(stat); kputs("\n"); }
            seen++;
            if (type == WLC_E_ESCAN_RESULT && stat == 8) found += escan_walk(ev + 72, l - 4 - 72, ap_line);
            if (type == WLC_E_ESCAN_RESULT && stat != 8) break;   /* anything but WLC_E_STATUS_PARTIAL ends the scan */
        }
        mdelay(5);
    }
    kputs("wifi scan done, "); kdec(found); kputs(" networks\n"); step(8); summary();
    return 1;
}


/* Join the network in wifi_cfg.h. The Pi's firmware has no supplicant (sup_wpa: -23, unsupported, on the real board),
   so the chip only associates and we do the WPA2 4-way handshake over EAPOL frames ourselves (arch/arm64/wpa.h), then
   hand it the pairwise and group keys; it encrypts in hardware from then on. The order is brcmfmac's with wpa_supplicant:
   infra, open auth, AES, WPA2-PSK, our RSN element, SET_SSID; message 1 in, message 2 out, message 3 in, message 4 out,
   then the keys. */
static void put32(unsigned char *b, unsigned v) { b[0] = (unsigned char)v; b[1] = (unsigned char)(v >> 8); b[2] = (unsigned char)(v >> 16); b[3] = (unsigned char)(v >> 24); }
#if WIFI_SSID_LEN > 0
#include "wpa.h"
static unsigned char mymac[6];
/* RSN element: version 1, group CCMP, one pairwise CCMP, one AKM PSK, no capabilities. Sent in association and msg 2. */
static const unsigned char rsn_ie[22] = {0x30, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0};
static unsigned char txf[512] __attribute__((aligned(64)));
/* One Ethernet frame out on the data channel: SDPCM header, a 4-byte BDC header (version 2, no offset), the frame. */
static int data_send(const unsigned char dst[6], unsigned type, const unsigned char *body, unsigned len) {
    unsigned n = SDPCM_HDRLEN + 4 + 14 + len;
    if (n > sizeof txf) return 0;
    wait_credit();
    wr16(txf, n); wr16(txf + 2, ~n & 0xffff); txf[4] = (unsigned char)seq++; txf[5] = SDPCM_DATA; txf[6] = 0; txf[7] = SDPCM_HDRLEN;
    txf[8] = txf[9] = txf[10] = txf[11] = 0;
    unsigned char *b = txf + SDPCM_HDRLEN; b[0] = 0x20; b[1] = b[2] = b[3] = 0;
    unsigned char *e = b + 4;
    for (int i = 0; i < 6; i++) { e[i] = dst[i]; e[6 + i] = mymac[i]; }
    e[12] = (unsigned char)(type >> 8); e[13] = (unsigned char)type;
    for (unsigned i = 0; i < len; i++) e[14 + i] = body[i];
    return cmd53(2, 0x8000, 1, txf, fw_padded(n));
}
/* brcmf_wsec_key_le: index, len, data[32], pad[18 words], algo, flags, pad[3], iv_initialized, pad, rxiv, pad[2], ea. */
static int set_key(unsigned index, const unsigned char *key, const unsigned char *ea, unsigned flags) {
    unsigned char k[164] = {0}; unsigned st = 0;
    put32(k, index); put32(k + 4, 16);
    for (int i = 0; i < 16; i++) k[8 + i] = key[i];
    put32(k + 112, 4);   /* CRYPTO_ALGO_AES_CCM */
    put32(k + 116, flags);
    if (ea) for (int i = 0; i < 6; i++) k[156 + i] = ea[i];
    if (!iovar("wsec_key", 1, k, sizeof k, &st) || st) { kputs("wifi key "); kdec(index); kputs(" status "); kx(st); kputs("\n"); return 0; }
    return 1;
}
static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
/* An EAPOL-Key reply (msg 2 or 4): copies the replay counter, sets our nonce and key data, signs it with the KCK. */
static int eapol_reply(const unsigned char aa[6], unsigned ver, unsigned info, const unsigned char replay[8],
                       const unsigned char *nonce, const unsigned char *kd, unsigned kdlen, const unsigned char kck[16]) {
    unsigned char m[99 + 32] = {0}, mic[20];
    unsigned body = 95 + kdlen;
    m[0] = (unsigned char)ver; m[1] = 3; m[2] = (unsigned char)(body >> 8); m[3] = (unsigned char)body;
    m[4] = 2; m[5] = (unsigned char)(info >> 8); m[6] = (unsigned char)info;
    for (int i = 0; i < 8; i++) m[9 + i] = replay[i];
    if (nonce) for (int i = 0; i < 32; i++) m[17 + i] = nonce[i];
    m[97] = (unsigned char)(kdlen >> 8); m[98] = (unsigned char)kdlen;
    for (unsigned i = 0; i < kdlen; i++) m[99 + i] = kd[i];
    wpa_hmac(kck, 16, m, 4 + body, 0, 0, mic);
    for (int i = 0; i < 16; i++) m[81 + i] = mic[i];
    return data_send(aa, 0x888e, m, 4 + body);
}
static int join(void) {
    unsigned st = 0; unsigned char b[80] = {0};
    if (!iovar("cur_etheraddr", 0, mymac, 6, &st) || st) { fail("mac"); return 0; }
    put32(b, 1); if (!wlc_ioctl(20, b, 4, &st) || st) { fail("infra"); return 0; }    /* WLC_SET_INFRA: infrastructure */
    put32(b, 0); if (!wlc_ioctl(22, b, 4, &st) || st) { fail("auth"); return 0; }     /* WLC_SET_AUTH: open system */
    put32(b, 4); if (!wlc_ioctl(134, b, 4, &st) || st) { fail("wsec"); return 0; }    /* WLC_SET_WSEC: AES */
    put32(b, 0x80); if (!iovar("wpa_auth", 1, b, 4, &st) || st) { kputs("wifi wpa_auth status "); kx(st); kputs("\n"); fail("wpa_auth"); return 0; }   /* WPA2_AUTH_PSK */
    if (!iovar("wpaie", 1, (void *)rsn_ie, sizeof rsn_ie, &st) || st) { kputs("wifi wpaie status "); kx(st); kputs("\n"); }   /* best effort: the firmware can build its own */
    for (unsigned i = 0; i < 80; i++) b[i] = 0;
    put32(b, WIFI_SSID_LEN); for (unsigned i = 0; i < WIFI_SSID_LEN; i++) b[4 + i] = wifi_ssid[i];
    kputs("wifi joining "); kputs((const char *)wifi_ssid); kputs("\n");
    if (!wlc_ioctl(26, b, 36, &st) || st) { kputs("wifi set ssid status "); kx(st); kputs("\n"); fail("set ssid"); return 0; }   /* WLC_SET_SSID */
    unsigned char aa[6], anonce[32], snonce[32], ptk[48]; int have_ptk = 0;
    {   /* our nonce: SHA-1 over the counter, our MAC and the SSID; fresh each boot because the counter is */
        unsigned long c; __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(c));
        wpa_sha1 h; wpa_sha1_init(&h); wpa_sha1_add(&h, (unsigned char *)&c, sizeof c); wpa_sha1_add(&h, mymac, 6);
        wpa_sha1_add(&h, wifi_ssid, WIFI_SSID_LEN); unsigned char d[20]; wpa_sha1_end(&h, d);
        for (int i = 0; i < 32; i++) snonce[i] = d[i % 20] ^ (unsigned char)(c >> (8 * (i % 8)));
    }
    unsigned shown = 0;
    for (unsigned t = 0; t < 3000; t++) {   /* up to 15 s: association and the four messages */
        unsigned off, l; if (!f2_read()) break;
        int ch = sdpcm_parse(frame, 1600, &off, &l);
        if (ch == SDPCM_EVENT && l > 4 + 72) {
            const unsigned char *ev = frame + off + 4;
            unsigned type = (unsigned)ev[28] << 24 | ev[29] << 16 | ev[30] << 8 | ev[31];
            unsigned stat = (unsigned)ev[32] << 24 | ev[33] << 16 | ev[34] << 8 | ev[35];
            unsigned reason = (unsigned)ev[36] << 24 | ev[37] << 16 | ev[38] << 8 | ev[39];
            if (type != WLC_E_ESCAN_RESULT && shown++ < 8) { kputs("wifi join event "); kdec(type); kputs(" status "); kdec(stat); kputs(" reason "); kdec(reason); kputs("\n"); }
            if (type == 0 && stat != 0) { kputs("wifi join refused\n"); break; }   /* SET_SSID failed */
        } else if (ch == SDPCM_DATA && l > 4 + 14 + 99) {
            const unsigned char *bd = frame + off, *e = bd + 4 + bd[3] * 4;
            if (be16(e + 12) != 0x888e) { mdelay(5); continue; }
            const unsigned char *k = e + 14;
            unsigned info = be16(k + 5), kdlen = be16(k + 97);
            if (k[1] != 3 || 99 + kdlen > l - 4 - 14) continue;
            if ((info & 0x0080) && !(info & 0x0100)) {   /* message 1: ack, no MIC */
                for (int i = 0; i < 6; i++) aa[i] = e[6 + i];
                for (int i = 0; i < 32; i++) anonce[i] = k[17 + i];
                wpa_ptk(wifi_pmk, aa, mymac, anonce, snonce, ptk); have_ptk = 1;
                kputs("wifi handshake 1 of 4\n");
                if (!eapol_reply(aa, k[0], 0x010a, k + 9, snonce, rsn_ie, sizeof rsn_ie, ptk)) { fail("msg2 send"); return 0; }
                kputs("wifi handshake 2 of 4 sent\n");
            } else if (have_ptk && (info & 0x0080) && (info & 0x0100) && (info & 0x0040)) {   /* message 3: ack, MIC, install */
                unsigned char m[99 + 256], mic[20];
                unsigned body = be16(k + 2); if (4 + body > sizeof m) continue;
                for (unsigned i = 0; i < 4 + body; i++) m[i] = k[i];
                for (int i = 0; i < 16; i++) m[81 + i] = 0;
                wpa_hmac(ptk, 16, m, 4 + body, 0, 0, mic);
                int micok = 1; for (int i = 0; i < 16; i++) if (mic[i] != k[81 + i]) micok = 0;
                if (!micok) { kputs("wifi handshake 3 MIC wrong (the passphrase does not match)\n"); fail("mic"); return 0; }
                kputs("wifi handshake 3 of 4, MIC good\n");
                unsigned char kd[256]; unsigned gtk_id = 1; const unsigned char *gtk = 0;
                if ((info & 0x1000) && kdlen >= 24 && kdlen <= 264 && wpa_unwrap(ptk + 16, k + 99, kdlen, kd)) {
                    for (unsigned i = 0; i + 2 <= kdlen - 8; ) {   /* KDEs: dd len 00-0f-ac 01 keyid 0 GTK */
                        unsigned tl = kd[i + 1];
                        if (kd[i] == 0xdd && tl >= 6 + 16 && kd[i + 2] == 0 && kd[i + 3] == 0x0f && kd[i + 4] == 0xac && kd[i + 5] == 1) { gtk_id = kd[i + 6] & 3; gtk = kd + i + 8; break; }
                        if (kd[i] == 0) break;
                        i += 2 + tl;
                    }
                }
                if (!eapol_reply(aa, k[0], 0x030a, k + 9, 0, 0, 0, ptk)) { fail("msg4 send"); return 0; }
                kputs("wifi handshake 4 of 4 sent\n");
                if (!set_key(0, ptk + 32, aa, 2)) { fail("pairwise key"); return 0; }   /* BRCMF_PRIMARY_KEY */
                if (gtk && !set_key(gtk_id, gtk, 0, 0)) { fail("group key"); return 0; }
                kputs(gtk ? "wifi joined, keys installed\n" : "wifi joined, no group key found\n");
                step(9); summary(); return 1;
            }
        }
        mdelay(5);
    }
    fail("join");
    return 0;
}
#else
static int join(void) { kputs("wifi: no network configured, scan only\n"); return 1; }
#endif

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
    /* brcmfmac's kso_init: KEEP_SDIO_ON in F1 SLEEPCSR (0x1000f), or the chip may sleep once the firmware runs and
       every access after that times out. CARDCTRL (CCCR 0xf1) bit 1: a card reset also resets the WLAN backplane. */
    { unsigned v = 0; cmd52(1, 0x1000f, 0, 0, &v); if (!(v & 1)) cmd52(1, 0x1000f, 1, v | 1, 0);
      v = 0; cmd52(0, 0xf1, 0, 0, &v); cmd52(0, 0xf1, 1, v | 2, 0); }
    /* The backplane (the chip's RAM and cores) only answers while its ALP clock runs: ask for it through CHIPCLKCSR
       (F1 0x1000e: ALP_AVAIL_REQ 0x08) and wait for ALP_AVAIL (0x40), like brcmfmac and
       Plan 9's ether4330. The fourth real-board run wrote 64 bytes and then got a general error on the next block. */
    if (!cmd52(1, 0x1000e, 1, 0x08, 0)) { fail("alp req"); return 0; }   /* ALP_AVAIL_REQ alone, as brcmfmac's htclk does; the sixth run with 0x28 (plus FORCE_HW_CLKREQ_OFF) lost the backplane again */
    for (unsigned n = 0;; n++) { unsigned v; if (cmd52(1, 0x1000e, 0, 0, &v) && (v & 0x40)) break; if (n > 100) { fail("alp"); return 0; } mdelay(5); }
    kputs("wifi alp clock up\n"); step(4);
    if (!fw_load()) return 0;
    if (!scan()) return 0;
    return join();
}
#else
int wifi_init(void) { return 0; }
#endif
