/* Bluetooth, slice BT1. The Pi 4's CYW43455 (the Wi-Fi chip) has a Bluetooth half that speaks HCI over a UART, in the
   H4 framing: one packet-type byte (1 command, 2 ACL data, 4 event), then the packet. On the Pi it hangs off the PL011
   (UART0, 0xFE201000) on GPIO 30-33 in ALT3: CTS, RTS, TX, RX. RTS/CTS flow control is on, as in Linux's Pi 4 device
   tree, so a burst the polled loop has not read yet waits in the radio instead of overrunning our FIFO; that is why the
   console moved to the mini UART (main.c). BT_ON is pin 128 of the firmware's GPIO expander (wifi.c's pi_exp_gpio).

   Bring-up follows Linux's btbcm.c: power, HCI Reset and Read Local Version at 115200, then the BCM4345C0 patch: Download
   Minidriver (0xFC2E), 50 ms, every record of the .hcd file sent as the HCI command it is (Write RAM 0xFC4C, the last one
   Launch RAM 0xFC4E), 250 ms while the patched firmware starts back at 115200, HCI Reset, Read Local Version again. Then
   the vendor baud command (0xFC18) to 921600, Set Event Mask with LE Meta (bit 61) on, and the radio is ready.
   `bt scan` runs an active LE scan for 5 seconds (a mouse often puts its name only in the scan response) and lists every
   address heard, once, with its name (AD type 0x09, else 0x08) and its latest RSSI.

   One command is in flight at a time and each has a deadline on the generic timer (cntpct, never main.c's `ticks`).
   No answer by then prints "bt: no answer from the radio" and Bluetooth stops; the rest of the kernel carries on.
   On QEMU's virt machine the radio is a second PL011 at 0x09040000, there only when QEMU gets a second -serial;
   tools/checks/arm64-bt-check.py puts a fake controller on it. BT2 adds connecting, pairing and HID over GATT. */
#include "bt.h"
#define REG(a) (*(volatile unsigned *)(unsigned long)(a))
void kputs(const char *s);
extern void (*cmd_out)(const char *s);   /* cmd.c: where the running command's output goes */
void term_output(int on);                /* main.c: lines go to the Terminal instead of the Console */
void ask_redraw(void);                   /* ask.c: the Terminal's prompt row, after lines printed between commands */
extern volatile int kprobe_armed, kprobe_faulted;   /* main.c: a data abort while armed is skipped, not fatal */
extern const unsigned char bt_fw_hcd[];  /* bt_fw.S: BCM4345C0.hcd, or nothing */
extern const unsigned bt_fw_hcd_len;

#ifdef PI_BUILD
#define BT_UART 0xFE201000UL
#define BT_CLK 48000000u                 /* the firmware's UART clock */
#define GPIO 0xFE200000UL
#define BT_ON 128
#define CR_ON (0x301u | 1u << 14 | 1u << 15)   /* UART, TX, RX, hardware RTS and CTS */
int pi_exp_gpio(unsigned pin, unsigned on);    /* wifi.c */
#else
#ifndef BT_UART_BASE
#define BT_UART_BASE 0x09040000UL        /* QEMU virt's second PL011 */
#endif
#define BT_UART BT_UART_BASE
#define BT_CLK 24000000u                 /* QEMU virt's PL011 clock; QEMU ignores the divisor anyway */
#define CR_ON 0x301u
#endif
#define U(o) REG(BT_UART + (o))
#define DR 0x00
#define FR 0x18
#define IBRD 0x24
#define FBRD 0x28
#define LCRH 0x2C
#define CR 0x30
#define IMSC 0x38
#define ICR 0x44
#define RXFE (1u << 4)
#define TXFF (1u << 5)
#define BUSY (1u << 3)
#define TXFE (1u << 7)

#define OP_RESET 0x0C03
#define OP_EVENT_MASK 0x0C01
#define OP_VERSION 0x1001
#define OP_SCAN_PARAMS 0x200B
#define OP_SCAN_ENABLE 0x200C
#define OP_MINIDRIVER 0xFC2E
#define OP_LAUNCH_RAM 0xFC4E
#define OP_BAUD 0xFC18
#define FAST_BAUD 921600u
#define CMD_MS 3000                      /* every command's deadline */
#define SCAN_MS 5000

enum { OFF, NO_RADIO, STARTING, READY, SCANNING, FAILED };
static int st;
static int pc;                           /* the next step of bring-up (0-12) or of a scan (20-24) */
static unsigned want;                    /* the opcode whose answer we wait for, 0 when none */
static unsigned long until;              /* that answer's deadline, or the end of a pause */
static unsigned char ret[16]; static unsigned retn;   /* the last Command Complete's return parameters, status first */
static unsigned fwpos, nrec, launched;   /* where the patch upload is */
static int to_term;                      /* the scan was typed at the Terminal: its results go there */
static char version[64];                 /* "hci 9 lmp 9 sub 0x6119 rev 0x183", once the radio has said it */
static const char *why = "";             /* what stopped Bluetooth, for `bt` */
static unsigned acl_in, rx_err, rx_junk; /* ACL packets heard (BT2 will want them), receive errors, bytes outside a packet */

static unsigned long now(void) { unsigned long t; __asm__ volatile ("isb\n mrs %0, cntpct_el0" : "=r"(t)); return t; }
static unsigned long ms(unsigned n) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return (f ? f : 54000000) / 1000 * n; }

/* ---- Lines out. A line built whole, then printed once: to the Terminal when a scan typed there is reporting. ---- */
static char line[112]; static unsigned ln;
static void put(const char *s) { while (*s && ln < sizeof line - 2) line[ln++] = *s++; }
static void put_dec(int v) { char t[12]; unsigned n = 11, u = v < 0 ? (unsigned)-v : (unsigned)v; t[n] = 0;
    do t[--n] = (char)('0' + u % 10); while (u /= 10); if (v < 0) t[--n] = '-'; put(t + n); }
static void put_hex(unsigned v) { char t[11]; unsigned n = 10; t[n] = 0; do t[--n] = "0123456789abcdef"[v & 15]; while (v >>= 4); put("0x"); put(t + n); }
static void say(void) {                  /* prints the line built so far, from the main loop */
    line[ln++] = '\n'; line[ln] = 0; ln = 0;
    if (to_term) term_output(1);
    kputs(line);
    if (to_term) { term_output(0); ask_redraw(); }
}
static void say_now(void) { line[ln++] = '\n'; line[ln] = 0; ln = 0; cmd_out(line); }   /* inside a typed command */
static void stop(const char *msg) {   /* a deadline passed: one plain line, plus a hint for a board with no serial cable */
    int stuck = !(U(FR) & TXFE);          /* bytes still in our FIFO: the radio never let them out (CTS, GPIO 30) */
    st = FAILED; why = msg; want = 0; put(msg); say();
    if (stuck) { put("bt: the command never left the UART, the radio held CTS"); say(); }
    to_term = 0;
}

/* ---- The UART ---- */
static void ux_baud(unsigned baud) {   /* off, last byte out, the new divisor, LCR_H (which latches it), on again */
    U(CR) = 0;
    for (unsigned long t0 = now(); (U(FR) & BUSY) && now() - t0 < ms(5);) {}
    unsigned div = (BT_CLK * 4 + baud / 2) / baud;   /* 64 * clock / (16 * baud): IBRD above 6 fraction bits */
    U(IBRD) = div >> 6; U(FBRD) = div & 63;
    U(LCRH) = 0x70;                      /* 8 bits, no parity, FIFOs on */
    U(CR) = CR_ON;
}
static unsigned char txb[260]; static unsigned txn, txi;
static void pump(void) { while (txi < txn && !(U(FR) & TXFF)) U(DR) = txb[txi++]; }   /* as much as the FIFO takes, no waiting */
static void cmd(unsigned op, const unsigned char *p, unsigned n) {
    txb[0] = 1; txb[1] = (unsigned char)op; txb[2] = (unsigned char)(op >> 8); txb[3] = (unsigned char)n;
    for (unsigned i = 0; i < n; i++) txb[4 + i] = p[i];
    txn = 4 + n; txi = 0; want = op; retn = 0; until = now() + ms(CMD_MS);
    pump();
}
static void pause_ms(unsigned n) { until = now() + ms(n); }

/* ---- What the radio heard: a short table, one row per address ---- */
#define DEVS 24
static struct { unsigned char a[6], type, full; signed char rssi; char name[30]; } dev[DEVS];
static unsigned ndev, nlost;
static void heard(const unsigned char *a, unsigned type, const unsigned char *d, unsigned n, signed char rssi) {
    unsigned k = 0;
    for (; k < ndev; k++) { unsigned j = 0; while (j < 6 && dev[k].a[j] == a[j]) j++; if (j == 6 && dev[k].type == type) break; }
    if (k == ndev) {
        if (ndev == DEVS) { nlost++; return; }
        for (unsigned j = 0; j < 6; j++) dev[k].a[j] = a[j];
        dev[k].type = (unsigned char)type; dev[k].full = 0; dev[k].name[0] = 0; dev[k].rssi = 127; ndev++;
    }
    if (rssi != 127) dev[k].rssi = rssi;   /* 127: the radio could not measure it */
    for (unsigned i = 0; i + 1 < n && d[i] && i + 1 + d[i] <= n; i += 1 + d[i]) {   /* AD structures: length, type, data */
        unsigned t = d[i + 1], len = d[i] - 1u;
        if (t != 0x09 && !(t == 0x08 && !dev[k].full)) continue;   /* a complete name beats a shortened one */
        unsigned m = 0;
        for (; m < len && m < sizeof dev[k].name - 1; m++) { unsigned char c = d[i + 2 + m]; dev[k].name[m] = c >= 32 && c < 127 ? (char)c : '?'; }
        dev[k].name[m] = 0; dev[k].full = t == 0x09;
    }
}
/* LE Advertising Report: the number of reports, then each one whole, back to back, the way BlueZ reads them:
   event type, address type, address (6, little endian), data length, data, RSSI. */
static void adv_reports(const unsigned char *p, unsigned n) {
    unsigned k = n ? p[0] : 0, i = 1;
    while (k-- && i + 9 <= n) {
        unsigned len = p[i + 8];
        if (i + 10 + len > n) break;
        heard(p + i + 2, p[i + 1], p + i + 9, len, (signed char)p[i + 9 + len]);
        i += 10 + len;
    }
}

/* ---- H4 in: a packet-type byte, a header, the body. Anything else is skipped a byte at a time until it lines up. ---- */
static unsigned char rx[260]; static unsigned rxn, rxneed, rxtype, rxhdr;
static void on_event(unsigned code, const unsigned char *p, unsigned n) {
    if (code == 0x0E && n >= 3 && want && (unsigned)(p[1] | p[2] << 8) == want) {        /* Command Complete: count, opcode, returns */
        retn = n - 3 < sizeof ret ? n - 3 : sizeof ret;
        for (unsigned i = 0; i < retn; i++) ret[i] = p[3 + i];
        want = 0; until = 0;                     /* answered: the next step may run at once */
    } else if (code == 0x0F && n >= 4 && want && (unsigned)(p[2] | p[3] << 8) == want) { /* Command Status: status, count, opcode */
        ret[0] = p[0]; retn = 1; want = 0; until = 0;
    } else if (code == 0x3E && n >= 2 && p[0] == 0x02 && st == SCANNING) adv_reports(p + 1, n - 1);
}
static void rx_byte(unsigned char c) {
    if (!rxtype) {
        if (c == 4 || c == 2) { rxtype = c; rxn = 0; rxhdr = 0; rxneed = c == 4 ? 2 : 4; } else rx_junk++;
        return;
    }
    if (rxn < sizeof rx) rx[rxn] = c;
    rxn++;
    if (!rxhdr && rxn == rxneed) { rxhdr = 1; rxneed += rxtype == 4 ? rx[1] : (unsigned)(rx[2] | rx[3] << 8); }
    if (rxhdr && rxn == rxneed) {
        if (rxtype == 4) on_event(rx[0], rx + 2, rx[1]); else acl_in++;
        rxtype = 0;
    }
}

/* ---- The steps. Each runs once the previous command has its answer and any pause is over. ---- */
static int patch_ok(void) {   /* the file is whole records: opcode, length, that many bytes, to the last byte */
    unsigned i = 0, n = bt_fw_hcd_len;
    while (i + 3 <= n) i += 3 + bt_fw_hcd[i + 2];
    return n && i == n;
}
static void put_version(void) {   /* Read Local Version: status, HCI version, revision, LMP version, maker, LMP subversion */
    ln = 0; put("hci "); put_dec(ret[1]); put(" lmp "); put_dec(ret[4]); put(" sub "); put_hex(ret[7] | ret[8] << 8u);
    put(" rev "); put_hex(ret[2] | ret[3] << 8u);
    for (unsigned i = 0; i <= ln; i++) version[i] = i < ln ? line[i] : 0;
    ln = 0;
}
static void step(void) {
    static const unsigned char baud[6] = { 0, 0, FAST_BAUD & 255, FAST_BAUD >> 8 & 255, FAST_BAUD >> 16 & 255, FAST_BAUD >> 24 };
    static const unsigned char mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x20 };   /* the default, plus LE Meta */
    static const unsigned char scan[7] = { 1, 0x30, 0, 0x30, 0, 0, 0 };   /* active, 30 ms interval and window, public, all */
    static const unsigned char on[2] = { 1, 0 }, off[2] = { 0, 0 }, launch[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    if (retn && ret[0]) {   /* the last command came back with an error status */
        ln = 0; put("bt: the radio refused "); put_hex(txb[1] | txb[2] << 8u); put(", status "); put_hex(ret[0]); say();
        retn = 0;
        if (st == SCANNING) st = READY; else { st = FAILED; why = "bt: the radio refused a command"; }
        to_term = 0; return;
    }
    switch (pc++) {
    case 0:
#ifdef PI_BUILD
        pi_exp_gpio(BT_ON, 0); pause_ms(10); return;   /* off a moment, so the chip starts from its ROM at 115200 */
#endif
    case 1:
#ifdef PI_BUILD
        pi_exp_gpio(BT_ON, 1); pause_ms(200); return;
#endif
    case 2: ux_baud(115200); cmd(OP_RESET, 0, 0); return;
    case 3: cmd(OP_VERSION, 0, 0); return;
    case 4:
        if (!bt_fw_hcd_len) { put("bt: no patch file, the radio runs its own firmware"); say(); pc = 10; step(); return; }
        if (!patch_ok()) { put("bt: patch file damaged, the radio runs its own firmware"); say(); pc = 10; step(); return; }
        put("bt: loading the radio's patch"); say();
        cmd(OP_MINIDRIVER, 0, 0); fwpos = nrec = launched = 0; return;
    case 5: pause_ms(50); return;
    case 6:
        if (fwpos < bt_fw_hcd_len) {   /* the next record, as the HCI command it already is */
            const unsigned char *r = bt_fw_hcd + fwpos;
            unsigned op = r[0] | r[1] << 8u;
            cmd(op, r + 3, r[2]); fwpos += 3u + r[2]; nrec++; launched = op == OP_LAUNCH_RAM;
            pc = 6; return;
        }
        if (!launched) { cmd(OP_LAUNCH_RAM, launch, 4); return; }
        /* fall through: the file's own last record was Launch RAM */
        pc++;
    case 7: pause_ms(250); put("bt: patch loaded, "); put_dec((int)nrec); put(" records"); say(); return;
    case 8: cmd(OP_RESET, 0, 0); return;
    case 9: cmd(OP_VERSION, 0, 0); return;
    case 10:
        put_version(); put("bt: version "); put(version); say();
        cmd(OP_BAUD, baud, 6); return;
    case 11: ux_baud(FAST_BAUD); pause_ms(10); return;
    case 12: cmd(OP_EVENT_MASK, mask, 8); return;
    case 13: st = READY; put("bt: ready, type bt scan to look for devices"); say(); return;
    case 20: cmd(OP_SCAN_PARAMS, scan, 7); return;
    case 21: cmd(OP_SCAN_ENABLE, on, 2); return;
    case 22: pause_ms(SCAN_MS); return;
    case 23: cmd(OP_SCAN_ENABLE, off, 2); return;
    case 24:
        for (unsigned k = 0; k < ndev; k++) {
            put("bt: "); put(dev[k].name[0] ? dev[k].name : "(no name)"); put("  rssi ");
            if (dev[k].rssi == 127) put("?"); else put_dec(dev[k].rssi);
            put("  addr ");
            for (int j = 5; j >= 0; j--) { char h[3] = { "0123456789abcdef"[dev[k].a[j] >> 4], "0123456789abcdef"[dev[k].a[j] & 15], 0 }; put(h); if (j) put(":"); }
            say();
        }
        put("bt: scan done, "); if (ndev) { put_dec((int)ndev); put(ndev == 1 ? " device" : " devices"); } else put("nothing heard");
        if (nlost) { put(", "); put_dec((int)nlost); put(" more not kept"); }
        say();
        st = READY; to_term = 0; return;
    }
}

void bt_init(void) {
#if defined(PI_BUILD) && !defined(CONSOLE_MINI_UART)
    st = OFF; why = "bt: off, the console has the radio's UART"; return;
#else
#ifdef PI_BUILD
    unsigned f = REG(GPIO + 0x0C);   /* GPFSEL3, GPIO 30-39: only 30-33 change; 34-39 are Wi-Fi's SDIO pins */
    REG(GPIO + 0x0C) = (f & ~0xFFFu) | 7u | 7u << 3 | 7u << 6 | 7u << 9;      /* ALT3: CTS0, RTS0, TXD0, RXD0 */
    REG(GPIO + 0xE8) = (REG(GPIO + 0xE8) & ~(15u << 28)) | 1u << 28;          /* pulls: 30 (CTS) up, 31 none */
    REG(GPIO + 0xEC) = (REG(GPIO + 0xEC) & ~15u) | 1u << 2;                   /* 32 none, 33 (RX) up; 34 on untouched */
    pc = 0;
#else
    kprobe_faulted = 0; kprobe_armed = 1;   /* QEMU virt faults on an address with no device behind it */
    unsigned id = U(0xFE0);                  /* PL011 PeriphID0 */
    kprobe_armed = 0;
    if (kprobe_faulted || (id & 0xFF) != 0x11) { st = NO_RADIO; why = "bt: no radio"; kputs("bt: no radio found at boot\n"); return; }
    pc = 2;
#endif
    U(CR) = 0; U(IMSC) = 0; U(ICR) = 0x7FF;
    st = STARTING; until = now();
#endif
}

void bt_poll(void) {
    if (st != STARTING && st != SCANNING) return;
    pump();
    for (unsigned k = 0; k < 512 && !(U(FR) & RXFE); k++) {
        unsigned d = U(DR);
        if (d & 0xF00) rx_err++;          /* framing, parity, break or overrun */
        rx_byte((unsigned char)d);
    }
    if (want) { if (now() > until) stop("bt: no answer from the radio"); return; }
    if (now() < until) return;
    step();
}

int bt_busy(void) { return st == STARTING || st == SCANNING; }

int bt_command(const char *q, unsigned n) {
    if (n < 2 || q[0] != 'b' || q[1] != 't' || (n > 2 && q[2] != ' ')) return 0;
    unsigned i = 2; while (i < n && q[i] == ' ') i++;
    int scan = n - i == 4 && q[i] == 's' && q[i + 1] == 'c' && q[i + 2] == 'a' && q[i + 3] == 'n';
    if (i < n && !scan) { put("bt: try bt, or bt scan"); say_now(); return 1; }
    if (scan && st == READY) {
        ndev = nlost = 0; to_term = 1; st = SCANNING; pc = 20; until = now();
        put("bt: scanning for 5 seconds"); say_now(); return 1;
    }
    switch (st) {
    case READY: put("bt: ready, version "); put(version); break;
    case SCANNING: put("bt: scanning"); break;
    case STARTING:
        if (pc == 6 && nrec) { put("bt: loading the radio's patch, record "); put_dec((int)nrec); }
        else put("bt: starting");
        break;
    default: put(why); break;
    }
    say_now();
    if (st == READY && (acl_in || rx_err || rx_junk)) {
        put("bt: acl in "); put_dec((int)acl_in); put(", rx errors "); put_dec((int)rx_err); put(", stray bytes "); put_dec((int)rx_junk); say_now();
    }
    return 1;
}
