/* Bluetooth, slices BT1 and BT2. The Pi 4's CYW43455 (the Wi-Fi chip) has a Bluetooth half that speaks HCI over a UART,
   in the H4 framing: one packet-type byte (1 command, 2 ACL data, 4 event), then the packet. On the Pi it hangs off the
   PL011 (UART0, 0xFE201000) on GPIO 30-33 in ALT3: CTS, RTS, TX, RX. RTS/CTS flow control is on, as in Linux's Pi 4
   device tree, so a burst the polled loop has not read yet waits in the radio instead of overrunning our FIFO; that is why
   the console moved to the mini UART (main.c). BT_ON is pin 128 of the firmware's GPIO expander (wifi.c's pi_exp_gpio).

   BT1, bring-up, follows Linux's btbcm.c: power, HCI Reset and Read Local Version at 115200, then the BCM4345C0 patch:
   Download Minidriver (0xFC2E), 50 ms, every record of the .hcd file sent as the HCI command it is (Write RAM 0xFC4C, the
   last one Launch RAM 0xFC4E), 250 ms while the patched firmware starts back at 115200, HCI Reset, Read Local Version
   again. Then the vendor baud command (0xFC18) to 921600, Set Event Mask with LE Meta (bit 61) on, and the radio is ready.
   `bt scan` runs an active LE scan for 5 seconds (a mouse often puts its name only in the scan response) and lists every
   address heard, once, with its name (AD type 0x09, else 0x08) and its latest RSSI.

   BT2, `bt pair`: the mouse from the last scan (the first name with "mouse" in it, else the strongest), as a HID over
   GATT boot mouse. LE Read Buffer Size (0x2002, or the shared Read Buffer Size when it says 0) sets how many ACL packets
   the radio holds; each send spends one and Number of Completed Packets gives them back. Read BD_ADDR for our own
   address, LE Create Connection (0x200D) with the address type the scan heard, LE Connection Complete: `bt: connected`.
   Then SMP on L2CAP channel 6, legacy Just Works (no input, no output, TK zero): Pairing Request and Response, our
   confirm c1(TK, Mrand) and random, the mouse's confirm checked against its random, STK = s1(TK, Srand, Mrand), and LE
   Start Encryption (0x2019) with it: Encryption Change, `bt: paired`. The mouse then hands over its LTK, EDIV and Rand;
   they stay in RAM only (no SD writes yet), so a reboot means pairing again. AES-128 is BearSSL's constant-time aes_ct.
   Then ATT on channel 4 (MTU 23): Find By Type Value for the HID service (0x1812), Read By Type over its characteristic
   declarations for Protocol Mode (0x2A4E) and Boot Mouse Input (0x2A33), Find Information for that report's CCCD
   (0x2902), Protocol Mode written to boot (0) with Write Command, notifications on with a Write Request: `bt: mouse
   ready`. Each notification is buttons, x, y and maybe the wheel, fed to main.c's kinput as the USB mouse's are. The
   mouse's own requests get answers so it never waits on us: ATT ones an Exchange MTU Response or an Error Response,
   a Connection Parameter Update Request (channel 5) an accept and LE Connection Update.

   One command is in flight at a time and each has a deadline on the generic timer (cntpct, never main.c's `ticks`), as
   does every wait on the mouse. No answer by then prints one plain line; Bluetooth stops or hangs up the link, and the
   rest of the kernel carries on. Nothing here prints a key, a random value or a confirm value.
   On QEMU's virt machine the radio is a second PL011 at 0x09040000, there only when QEMU gets a second -serial;
   tools/checks/arm64-bt-check.py puts a fake controller and a fake mouse on it. */
#include "bt.h"
#include "rng.h"
#include "bearssl_block.h"
#define REG(a) (*(volatile unsigned *)(unsigned long)(a))
void kputs(const char *s);
extern void (*cmd_out)(const char *s);   /* cmd.c: where the running command's output goes */
void term_output(int on);                /* main.c: lines go to the Terminal instead of the Console */
void ask_redraw(void);                   /* ask.c: the Terminal's prompt row, after lines printed between commands */
void kinput(unsigned type, unsigned code, int value);   /* main.c: one Linux input event (EV_KEY, EV_REL, EV_SYN) */
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

#define OP_DISCONNECT 0x0406
#define OP_RESET 0x0C03
#define OP_EVENT_MASK 0x0C01
#define OP_VERSION 0x1001
#define OP_BUFSIZE 0x1005
#define OP_BDADDR 0x1009
#define OP_LE_BUFSIZE 0x2002
#define OP_SCAN_PARAMS 0x200B
#define OP_SCAN_ENABLE 0x200C
#define OP_CONNECT 0x200D
#define OP_CONNECT_CANCEL 0x200E
#define OP_CONN_UPDATE 0x2013
#define OP_ENCRYPT 0x2019
#define OP_MINIDRIVER 0xFC2E
#define OP_LAUNCH_RAM 0xFC4E
#define OP_BAUD 0xFC18
#define FAST_BAUD 921600u
#define CMD_MS 3000                      /* every command's deadline */
#define SCAN_MS 5000
#define LINK_MS 10000                    /* the most we wait on the mouse for any one step */
#define FOREVER (~0UL)

enum { OFF, NO_RADIO, STARTING, READY, SCANNING, FAILED, PAIRING, MOUSE };
static int st;
static int pc;                           /* the next step: bring-up (0-13), a scan (20-24), pairing (30-60) */
static unsigned want;                    /* the opcode whose answer we wait for, 0 when none */
static int by_step;                      /* that command was sent by a step, so its answer runs the next one */
static unsigned last_op;                 /* the last command sent, for the "refused" line */
static unsigned long until;              /* the next step's time: the end of a pause, or a deadline on the mouse */
static unsigned long cmd_until;          /* the command in flight's deadline */
static unsigned char ret[16]; static unsigned retn;   /* the last Command Complete's return parameters, status first */
static unsigned fwpos, nrec, launched;   /* where the patch upload is */
static int to_term;                      /* the command was typed at the Terminal: its lines go there */
static char version[64];                 /* "hci 9 lmp 9 sub 0x6119 rev 0x183", once the radio has said it */
static const char *why = "";             /* what stopped Bluetooth, for `bt` */
static unsigned acl_in, rx_err, rx_junk; /* ACL packets heard, receive errors, bytes outside a packet */

static unsigned long now(void) { unsigned long t; __asm__ volatile ("isb\n mrs %0, cntpct_el0" : "=r"(t)); return t; }
static unsigned long ms(unsigned n) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return (f ? f : 54000000) / 1000 * n; }
static void wipe(void *p, unsigned n) { for (unsigned i = 0; i < n; i++) ((volatile unsigned char *)p)[i] = 0; }

/* ---- Lines out. A line built whole, then printed once: to the Terminal when a command typed there is reporting. ---- */
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

/* ---- The UART. Whole packets queue in txb behind what is still going out; pump() feeds the FIFO without waiting. ---- */
static void ux_baud(unsigned baud) {   /* off, last byte out, the new divisor, LCR_H (which latches it), on again */
    U(CR) = 0;
    for (unsigned long t0 = now(); (U(FR) & BUSY) && now() - t0 < ms(5);) {}
    unsigned div = (BT_CLK * 4 + baud / 2) / baud;   /* 64 * clock / (16 * baud): IBRD above 6 fraction bits */
    U(IBRD) = div >> 6; U(FBRD) = div & 63;
    U(LCRH) = 0x70;                      /* 8 bits, no parity, FIFOs on */
    U(CR) = CR_ON;
}
static unsigned char txb[640]; static unsigned txn, txi;
static void pump(void) { while (txi < txn && !(U(FR) & TXFF)) U(DR) = txb[txi++]; }
static unsigned char *tx_room(unsigned n) {   /* room for a packet of n bytes at the end of the queue, or 0 */
    if (txi == txn) txi = txn = 0;
    if (txn + n > sizeof txb) { for (unsigned i = txi; i < txn; i++) txb[i - txi] = txb[i]; txn -= txi; txi = 0; }
    if (txn + n > sizeof txb) return 0;
    txn += n; return txb + txn - n;
}
static void send_cmd(unsigned op, const unsigned char *p, unsigned n, int stepped) {
    unsigned char *b = tx_room(4 + n);
    if (!b) { stop("bt: the send queue is full"); return; }
    b[0] = 1; b[1] = (unsigned char)op; b[2] = (unsigned char)(op >> 8); b[3] = (unsigned char)n;
    for (unsigned i = 0; i < n; i++) b[4 + i] = p[i];
    want = last_op = op; by_step = stepped; cmd_until = now() + ms(CMD_MS);
    if (stepped) retn = 0;
    pump();
}
static void cmd(unsigned op, const unsigned char *p, unsigned n) { send_cmd(op, p, n, 1); }
static void pause_ms(unsigned n) { until = now() + ms(n); }
static unsigned pend_op, pend_n; static unsigned char pend_p[14];   /* one command the link needs, sent when none is in flight */

/* ---- What the radio heard: a short table, one row per address ---- */
#define DEVS 64   /* a busy room: the first 24 filled in 5 seconds and the mouse was one of the six not kept */
static struct { unsigned char a[6], type, full, hid; signed char rssi; unsigned char evts, adfl; unsigned short app, co; char name[30]; } dev[DEVS];   /* evts: bit per advertising event type heard (0 connectable, 1 directed, 2 scannable only, 3 not connectable); adfl: its Flags byte */   /* hid: 1 advertises the HID service, 2 says it is a mouse; app: its Appearance, co: its maker's Bluetooth company id */
static unsigned ndev, nlost;
static void heard(unsigned evt, const unsigned char *a, unsigned type, const unsigned char *d, unsigned n, signed char rssi) {
    unsigned k = 0;
    for (; k < ndev; k++) { unsigned j = 0; while (j < 6 && dev[k].a[j] == a[j]) j++; if (j == 6 && dev[k].type == type) break; }
    if (k == ndev) {
        if (ndev == DEVS) {   /* full: a device that advertises as a mouse or HID takes the place of the weakest one that does not */
            int hid_now = 0;
            for (unsigned i = 0; i + 1 < n && d[i] && i + 1 + d[i] <= n; i += 1 + d[i]) {
                unsigned t = d[i + 1], len = d[i] - 1u;
                if ((t == 0x02 || t == 0x03) && !(len & 1u)) for (unsigned m = 0; m < len; m += 2) if ((d[i + 2 + m] | d[i + 3 + m] << 8u) == 0x1812u) hid_now = 1;
                if (t == 0x19 && len >= 2) { unsigned ap = d[i + 2] | d[i + 3] << 8u; if (ap >= 0x03C0u && ap <= 0x03CFu) hid_now = 1; }
            }
            int w = -1;
            if (hid_now) for (unsigned x = 0; x < ndev; x++) if (!dev[x].hid && (w < 0 || dev[x].rssi < dev[w].rssi)) w = (int)x;
            if (w < 0) { nlost++; return; }
            k = (unsigned)w; ndev--; nlost++;   /* reuse that row: it is counted lost, the new device is counted heard just below */
        }
        for (unsigned j = 0; j < 6; j++) dev[k].a[j] = a[j];
        dev[k].type = (unsigned char)type; dev[k].full = 0; dev[k].hid = 0; dev[k].evts = dev[k].adfl = 0; dev[k].app = dev[k].co = 0; dev[k].name[0] = 0; dev[k].rssi = 127; ndev++;
    }
    if (rssi != 127) dev[k].rssi = rssi;   /* 127: the radio could not measure it */
    if (evt < 4) dev[k].evts |= (unsigned char)(1u << evt);
    for (unsigned i = 0; i + 1 < n && d[i] && i + 1 + d[i] <= n; i += 1 + d[i]) {   /* AD structures: length, type, data */
        unsigned t = d[i + 1], len = d[i] - 1u;
        /* Most mice and keyboards advertise no name at all, but they say what they are: the HID service (0x1812) in a 16 bit
           UUID list, or an Appearance (0x19) of 0x03C0 to 0x03CF, HID, with 0x03C2 the mouse. A tag in the scan list and a
           first pick for `bt pair` find the mouse among the phones and watches. */
        if ((t == 0x02 || t == 0x03) && !(len & 1u)) { for (unsigned m = 0; m < len; m += 2) if ((d[i + 2 + m] | d[i + 3 + m] << 8u) == 0x1812u && !dev[k].hid) dev[k].hid = 1; }
        if (t == 0x19 && len >= 2) { unsigned ap = d[i + 2] | d[i + 3] << 8u; dev[k].app = (unsigned short)ap; if (ap == 0x03C2u) dev[k].hid = 2; else if (ap >= 0x03C0u && ap <= 0x03CFu && !dev[k].hid) dev[k].hid = 1; }
        if (t == 0x01 && len >= 1) dev[k].adfl = d[i + 2];
        if (t == 0xFF && len >= 2) dev[k].co = (unsigned short)(d[i + 2] | d[i + 3] << 8u);   /* manufacturer data starts with the maker's company id */
        if (t != 0x09 && !(t == 0x08 && !dev[k].full)) continue;   /* a complete name beats a shortened one */
        unsigned m = 0;
        for (; m < len && m < sizeof dev[k].name - 1; m++) { unsigned char c = d[i + 2 + m]; dev[k].name[m] = c >= 32 && c < 127 ? (char)c : '?'; }
        dev[k].name[m] = 0; dev[k].full = t == 0x09;
    }
}
/* ---- The list in plain words: what a device says it is, how close it is, mice first, numbered for `bt pair N` ---- */
static int has(const char *name, const char *w, unsigned wn);
static const char *kind_word(unsigned k) {   /* from its Appearance, else its HID service, else its maker; 0 when it tells us nothing */
    unsigned a = dev[k].app;
    if (a == 0x03C2) return "Mouse";
    if (a == 0x03C1) return "Keyboard";
    if (a == 0x03C4) return "Game controller";
    if (a >= 0x03C0 && a <= 0x03CF) return "Input device";
    if (dev[k].hid) return "Mouse or keyboard";
    switch (a >> 6) { case 1: return "Phone"; case 2: return "Computer"; case 3: return "Watch"; case 5: return "Display"; case 6: return "Remote"; case 8: return "Tag"; case 10: return "Media player"; case 49: return "Earbuds"; default: break; }
    switch (dev[k].co) {
    case 0x004C: return "Apple device";      case 0x0006: return "Microsoft device"; case 0x00E0: return "Google device";
    case 0x0075: return "Samsung device";    case 0x012D: return "Sony device";      case 0x01DA: return "Logitech device";
    case 0x009E: return "Bose device";       case 0x038F: return "Xiaomi device";    case 0x0087: return "Garmin device";
    default: return 0;
    }
}
static const char *near_word(int rssi) { return rssi == 127 ? "signal unknown" : rssi >= -45 ? "very close" : rssi >= -60 ? "near" : rssi >= -75 ? "in the room" : "far away"; }
static unsigned shown[DEVS], nshown;   /* the last list's order: row N of `bt scan` is dev[shown[N - 1]] */
static int rank(unsigned k) {          /* higher lists first: a mouse, then anything HID, then a named device, then one that says what it is */
    int g = dev[k].hid == 2 ? 4 : dev[k].hid ? 3 : dev[k].name[0] ? 2 : kind_word(k) ? 1 : 0;
    return g * 1000 + (dev[k].rssi == 127 ? 0 : dev[k].rssi + 128);
}

/* LE Advertising Report: the number of reports, then each one whole, back to back, the way BlueZ reads them:
   event type, address type, address (6, little endian), data length, data, RSSI. */
static void adv_reports(const unsigned char *p, unsigned n) {
    unsigned k = n ? p[0] : 0, i = 1;
    while (k-- && i + 9 <= n) {
        unsigned len = p[i + 8];
        if (i + 10 + len > n) break;
        heard(p[i], p + i + 2, p[i + 1], p + i + 9, len, (signed char)p[i + 9 + len]);
        i += 10 + len;
    }
}

/* ---- The link to the mouse ---- */
static int conn, closing;                /* a link is up; we are hanging it up after a failure */
static int tries;                        /* connection attempts this `bt pair` */
static unsigned long conn_t0;            /* when the link came up, to say how long it lived */
static int scan_later;                   /* `bt scan` was typed while the radio was still starting: scan the moment it is ready */
static unsigned handle;                  /* its connection handle */
void wifi_hold(void); void wifi_release(void);   /* wifi.c: the shared antenna is Bluetooth's while a mouse pairs */
static unsigned char me[6], peer[6], peer_type;   /* our address and the mouse's, little endian, as the radio gives them */
static char peer_name[30];
static unsigned credits;                 /* ACL packets the radio can still take */
#define AQ 8
static unsigned char aq[AQ][32], aqn[AQ]; static unsigned aqh, aqt;   /* ACL packets waiting for a credit */
static void acl_flush(void) {
    while (aqh != aqt && credits) {
        unsigned char *b = tx_room(aqn[aqh % AQ]);
        if (!b) break;
        for (unsigned i = 0; i < aqn[aqh % AQ]; i++) b[i] = aq[aqh % AQ][i];
        wipe(aq[aqh % AQ], 32); aqh++; credits--;
    }
    pump();
}
static void l2_send(unsigned cid, const unsigned char *p, unsigned n) {   /* one L2CAP frame in one ACL packet (n <= 23) */
    if (!conn || n > 23 || aqt - aqh == AQ) return;   /* a full queue drops it; the step waiting on its answer times out */
    unsigned char *b = aq[aqt % AQ];
    b[0] = 2; b[1] = (unsigned char)handle; b[2] = (unsigned char)(handle >> 8 & 0x0F);   /* PB 00: first, non-flushable */
    b[3] = (unsigned char)(n + 4); b[4] = 0; b[5] = (unsigned char)n; b[6] = 0; b[7] = (unsigned char)cid; b[8] = 0;
    for (unsigned i = 0; i < n; i++) b[9 + i] = p[i];
    aqn[aqt % AQ] = (unsigned char)(9 + n); aqt++;
    acl_flush();
}
static void hang_up(void) {   /* Disconnect, reason 0x13 (we ended it), once nothing else is in flight */
    pend_op = OP_DISCONNECT; pend_p[0] = (unsigned char)handle; pend_p[1] = (unsigned char)(handle >> 8); pend_p[2] = 0x13; pend_n = 3;
}
static void fail(const char *msg) {   /* one plain line (msg, or the line built so far), then hang up if there is a link */
    wifi_release();
    if (msg) put(msg);
    say(); to_term = 0;
    by_step = 0;                          /* a command still in flight no longer runs a step */
    if (conn) { hang_up(); closing = 1; pc = 60; until = now() + ms(CMD_MS * 2); }
    else { st = READY; until = FOREVER; }
}

/* SMP, LE legacy pairing, Just Works. Every value is little endian, as it travels; e() is AES-128 on the spec's
   most-significant-first numbers, so it reverses key, input and output (Core spec Vol 3 Part H 2.2.1). */
static const unsigned char tk[16];       /* Just Works: TK is zero */
static unsigned char preq[7], pres[7], mrand[16], sconf[16], keysize;
static int smp;                          /* 1 request sent, 2 our confirm sent, 3 our random sent, 4 paired */
static unsigned char ltk[16], krand[8], kaddr[6], ktype; static unsigned kediv, keys;   /* the mouse's keys: RAM only */
static void smp_e(const unsigned char *k, const unsigned char *in, unsigned char *out) {
    unsigned char key[16], b[16], iv[16] = {0};
    br_aes_ct_cbcenc_keys c;
    for (int i = 0; i < 16; i++) { key[i] = k[15 - i]; b[i] = in[15 - i]; }
    br_aes_ct_cbcenc_init(&c, key, 16);
    br_aes_ct_cbcenc_run(&c, iv, b, 16);   /* one block, zero IV: plain AES */
    for (int i = 0; i < 16; i++) out[i] = b[15 - i];
    wipe(key, 16); wipe(b, 16); wipe(&c, sizeof c);
}
static void smp_c1(const unsigned char *r, unsigned char *out) {   /* the confirm value for random r (2.2.3) */
    unsigned char p[16];
    p[0] = 0; p[1] = peer_type & 1;      /* p1 = pres || preq || rat || iat; we connect with our public address */
    for (int i = 0; i < 7; i++) { p[2 + i] = preq[i]; p[9 + i] = pres[i]; }
    for (int i = 0; i < 16; i++) p[i] ^= r[i];
    smp_e(tk, p, out);
    for (int i = 0; i < 6; i++) { p[i] = peer[i]; p[6 + i] = me[i]; }   /* p2 = padding || ia || ra */
    p[12] = p[13] = p[14] = p[15] = 0;
    for (int i = 0; i < 16; i++) out[i] ^= p[i];
    smp_e(tk, out, out);
    wipe(p, 16);
}
static unsigned char stk[16];
static void smp_send(const unsigned char *p, unsigned n) { l2_send(6, p, n); until = now() + ms(LINK_MS); }
static void on_smp(const unsigned char *p, unsigned n) {
    if (st != PAIRING || closing || !n) return;
    unsigned char b[17], chk[16];
    if (p[0] == 0x02 && n == 7 && smp == 1) {             /* Pairing Response */
        for (int i = 0; i < 7; i++) pres[i] = p[i];
        keysize = p[4] < 16 ? p[4] : 16;
        if (keysize < 7) { b[0] = 0x05; b[1] = 0x06; l2_send(6, b, 2); fail("bt: pairing failed, the mouse wants a short key"); return; }
        if (!tls_entropy(mrand, 16)) { fail("bt: pairing failed, no random numbers"); return; }
        b[0] = 0x03; smp_c1(mrand, b + 1); smp_send(b, 17); smp = 2;
    } else if (p[0] == 0x03 && n == 17 && smp == 2) {      /* Pairing Confirm: the mouse's, kept to check its random */
        for (int i = 0; i < 16; i++) sconf[i] = p[1 + i];
        b[0] = 0x04; for (int i = 0; i < 16; i++) b[1 + i] = mrand[i];
        smp_send(b, 17); smp = 3;
    } else if (p[0] == 0x04 && n == 17 && smp == 3) {      /* Pairing Random: its confirm must be c1 of this random */
        smp_c1(p + 1, chk);
        unsigned d = 0; for (int i = 0; i < 16; i++) d |= chk[i] ^ sconf[i];
        if (d) {
            b[0] = 0x05; b[1] = 0x04; l2_send(6, b, 2);   /* Pairing Failed: Confirm Value Failed */
            wipe(mrand, 16); wipe(chk, 16);
            fail("bt: pairing failed, the mouse's confirm value is wrong"); return;
        }
        for (int i = 0; i < 8; i++) { b[i] = mrand[i]; b[8 + i] = p[1 + i]; }   /* s1: r' = Srand' || Mrand' */
        smp_e(tk, b, stk);
        for (unsigned i = keysize; i < 16; i++) stk[i] = 0;
        wipe(mrand, 16); wipe(chk, 16); wipe(b, 17);
        smp = 4; pc = 42; until = 0;
    } else if (p[0] == 0x05 && n >= 2) {                   /* Pairing Failed, from the mouse */
        put("bt: pairing failed, the mouse said no, reason "); put_hex(p[1]); fail(0);
    } else if (p[0] == 0x06 && n == 17) {                  /* Encryption Information: the LTK */
        for (int i = 0; i < 16; i++) ltk[i] = p[1 + i];
        keys |= 1;
    } else if (p[0] == 0x07 && n == 11) {                  /* Master Identification: EDIV and Rand */
        kediv = p[1] | p[2] << 8u;
        for (int i = 0; i < 8; i++) krand[i] = p[3 + i];
        for (int i = 0; i < 6; i++) kaddr[i] = peer[i];
        ktype = peer_type; keys |= 2;
    }
    wipe(b, 17); wipe(chk, 16);
}

/* ATT. Our side is a client of the mouse's GATT database; to the mouse's own requests our database is empty. */
static unsigned svc[4][2], nsvc, cur, pm_vh, boot_vh, next_decl, cccd, held;
static int si, gs;                       /* which HID service; 1 services, 2 characteristics, 3 descriptors, 4 CCCD write */
static void att_req(const unsigned char *p, unsigned n) { l2_send(4, p, n); until = now() + ms(LINK_MS); }
static void find_chars(void) {           /* Read By Type: characteristic declarations from cur to the service's end */
    unsigned e = svc[si][1];
    unsigned char q[7] = { 0x08, (unsigned char)cur, (unsigned char)(cur >> 8), (unsigned char)e, (unsigned char)(e >> 8), 0x03, 0x28 };
    gs = 2; att_req(q, 7);
}
static void find_cccd(void) {            /* Find Information between the report's value and the next declaration */
    unsigned e = next_decl ? next_decl - 1 : svc[si][1];
    if (e < cur) { fail("bt: the mouse's report cannot be turned on"); return; }
    unsigned char q[5] = { 0x04, (unsigned char)cur, (unsigned char)(cur >> 8), (unsigned char)e, (unsigned char)(e >> 8) };
    gs = 3; att_req(q, 5);
}
static void next_service(void) {
    if (++si >= (int)nsvc) { fail("bt: no boot mouse report on this device"); return; }
    cur = svc[si][0]; pm_vh = boot_vh = next_decl = 0;
    find_chars();
}
static void mouse_report(const unsigned char *r, unsigned n) {   /* boot mouse: buttons, x, y, maybe the wheel */
    if (n < 3) return;
    if (r[1]) kinput(2, 0, (signed char)r[1]);
    if (r[2]) kinput(2, 1, (signed char)r[2]);
    if (n > 3 && r[3]) kinput(2, 8, (signed char)r[3]);   /* REL_WHEEL: main.c has no reader for it yet */
    for (unsigned b = 0; b < 3; b++) if ((r[0] ^ held) >> b & 1) kinput(1, 272 + b, r[0] >> b & 1);   /* BTN_LEFT, RIGHT, MIDDLE */
    held = r[0] & 7u;
    kinput(0, 0, 0);
}
static void on_att(const unsigned char *p, unsigned n) {
    /* the requests the mouse may send us: Find Information, Find By Type, Read By Type, Read, Read Blob, Read Multiple,
       Read By Group, Write, Prepare and Execute Write, Read Multiple Variable */
    static const unsigned long reqs = 1UL << 0x04 | 1UL << 0x06 | 1UL << 0x08 | 1UL << 0x0A | 1UL << 0x0C | 1UL << 0x0E |
                                      1UL << 0x10 | 1UL << 0x12 | 1UL << 0x16 | 1UL << 0x18 | 1UL << 0x20;
    if (!n) return;
    unsigned op = p[0];
    if (op == 0x1B) { if (n >= 3 && boot_vh && (unsigned)(p[1] | p[2] << 8) == boot_vh && smp == 4) mouse_report(p + 3, n - 3); return; }
    if (op == 0x1D) { unsigned char c = 0x1E; l2_send(4, &c, 1); return; }                 /* an indication: confirm it */
    if (op == 0x02) { unsigned char r[3] = { 0x03, 23, 0 }; l2_send(4, r, 3); return; }    /* Exchange MTU: we stay at 23 */
    if (op < 64 && (reqs >> op & 1)) {   /* Error Response: Attribute Not Found to a search, Request Not Supported to the rest */
        int find = op == 0x04 || op == 0x06 || op == 0x08 || op == 0x10;
        unsigned char r[5] = { 0x01, (unsigned char)op, n >= 3 ? p[1] : 0, n >= 3 ? p[2] : 0, (unsigned char)(find ? 0x0A : 0x06) };
        l2_send(4, r, 5); return;
    }
    if (st != PAIRING || closing) return;
    unsigned last = 0;
    if (gs == 1) {                       /* the HID services: Find By Type Value Response, a start and an end each */
        if (op == 0x07) {
            for (unsigned i = 1; i + 4 <= n && nsvc < 4; i += 4) { svc[nsvc][0] = p[i] | p[i + 1] << 8u; svc[nsvc][1] = p[i + 2] | p[i + 3] << 8u; nsvc++; }
            si = -1; next_service();
        } else if (op == 0x01) fail("bt: no mouse service on this device");
    } else if (gs == 2) {                /* characteristic declarations: handle, properties, value handle, UUID */
        if (op == 0x09 && n >= 2 && p[1] >= 7) {
            unsigned len = p[1];
            for (unsigned i = 2; i + len <= n; i += len) {
                unsigned h = p[i] | p[i + 1] << 8u, vh = p[i + 3] | p[i + 4] << 8u, uuid = len == 7 ? (p[i + 5] | p[i + 6] << 8u) : 0;
                if (boot_vh && !next_decl && h > boot_vh) next_decl = h;
                if (uuid == 0x2A4E) pm_vh = vh;
                if (uuid == 0x2A33 && !boot_vh) boot_vh = vh;
                last = h;
            }
            if (last && last < svc[si][1]) { cur = last + 1; find_chars(); return; }
        } else if (op != 0x01) return;
        if (!boot_vh) { next_service(); return; }   /* the last page: Attribute Not Found, or the service's end */
        cur = boot_vh + 1; find_cccd();
    } else if (gs == 3) {                /* descriptors: handle and UUID, 16 bit (format 1) or 128 bit (format 2) */
        unsigned e = next_decl ? next_decl - 1 : svc[si][1];
        if (op == 0x05 && n >= 2) {
            unsigned w = p[1] == 1 ? 4 : 18;
            for (unsigned i = 2; i + w <= n; i += w) { unsigned h = p[i] | p[i + 1] << 8u; if (w == 4 && (p[i + 2] | p[i + 3] << 8u) == 0x2902 && !cccd) cccd = h; last = h; }
            if (!cccd && last && last < e) { cur = last + 1; find_cccd(); return; }
        } else if (op != 0x01) return;
        if (!cccd) { fail("bt: the mouse's report cannot be turned on"); return; }
        if (pm_vh) { unsigned char w[4] = { 0x52, (unsigned char)pm_vh, (unsigned char)(pm_vh >> 8), 0 }; l2_send(4, w, 4); }   /* Protocol Mode: boot */
        unsigned char q[5] = { 0x12, (unsigned char)cccd, (unsigned char)(cccd >> 8), 1, 0 };   /* notifications on */
        gs = 4; att_req(q, 5);
    } else if (gs == 4) {
        if (op == 0x13) {
            put("bt: mouse ready"); say();
            st = MOUSE; gs = 5; until = FOREVER; wifi_release(); to_term = 0;   /* Wi-Fi back first, so its lines land in the Terminal */
        } else if (op == 0x01 && n >= 5) { put("bt: the mouse would not send reports, error "); put_hex(p[4]); fail(0); }
    }
}
static void on_sig(const unsigned char *p, unsigned n) {   /* LE signalling: code, identifier, length, data */
    if (n < 4) return;
    if (p[0] == 0x12 && n >= 12) {       /* Connection Parameter Update Request: accept, then ask the radio for it */
        unsigned char r[6] = { 0x13, p[1], 2, 0, 0, 0 };
        l2_send(5, r, 6);
        if (pend_op != OP_DISCONNECT) {
            pend_op = OP_CONN_UPDATE; pend_n = 14; pend_p[0] = (unsigned char)handle; pend_p[1] = (unsigned char)(handle >> 8);
            for (int i = 0; i < 8; i++) pend_p[2 + i] = p[4 + i];   /* interval min and max, latency, timeout */
            for (int i = 10; i < 14; i++) pend_p[i] = 0;             /* connection event lengths */
        }
    } else if (p[0] == 0x06 || p[0] == 0x14 || p[0] == 0x17 || p[0] == 0x19) {   /* any other request: Command Reject */
        unsigned char r[6] = { 0x01, p[1], 2, 0, 0, 0 };
        l2_send(5, r, 6);
    }
}
/* An ACL packet: handle and flags, length, data. L2CAP frames can arrive in pieces: the first one carries the length. */
static unsigned char l2[80]; static unsigned l2n, l2want;
static void on_acl(const unsigned char *p, unsigned n) {
    acl_in++;
    if (n < 4) return;
    unsigned h = (p[0] | p[1] << 8u) & 0xFFF, pb = p[1] >> 4 & 3, len = p[2] | p[3] << 8u;
    if (!conn || h != handle || 4 + len > n) return;
    if (pb != 1) { l2n = 0; l2want = len >= 4 ? 4 + (p[4] | p[5] << 8u) : 0; }   /* a start */
    if (!l2want || l2n + len > sizeof l2 || l2want > sizeof l2) { l2want = 0; return; }   /* too big for anything we read */
    for (unsigned i = 0; i < len; i++) l2[l2n++] = p[4 + i];
    if (l2n < l2want) return;
    unsigned cid = l2[2] | l2[3] << 8u, m = l2want - 4;
    l2want = 0;
    if (cid == 4) on_att(l2 + 4, m); else if (cid == 5) on_sig(l2 + 4, m); else if (cid == 6) on_smp(l2 + 4, m);
    wipe(l2, sizeof l2);
}
static void connected(const unsigned char *p, unsigned n) {   /* LE Connection Complete: status, handle, role, peer */
    if (n < 18) return;
    unsigned h = (p[1] | p[2] << 8u) & 0xFFF;
    if (st != PAIRING || (pc != 35 && pc != 36) || conn) {   /* 35: the event beat our deadline step */   /* a link nobody waits for (one that came up as we cancelled): hang it up */
        if (!p[0] && !pend_op) { pend_op = OP_DISCONNECT; pend_p[0] = (unsigned char)h; pend_p[1] = (unsigned char)(h >> 8); pend_p[2] = 0x13; pend_n = 3; }
        return;
    }
    if (p[0]) { put("bt: could not connect, status "); put_hex(p[0]); fail(0); return; }
    conn = 1; handle = h; peer_type = p[4];
    for (int i = 0; i < 6; i++) peer[i] = p[5 + i];
    conn_t0 = now();
    put("bt: connected, link interval "); { unsigned iv = (p[11] | p[12] << 8u) * 125u; put_dec((int)(iv / 100)); put("."); put_dec((int)(iv / 10 % 10)); } put(" ms"); say();
    pc = 40; until = 0;
}
static void disconnected(const unsigned char *p, unsigned n) {   /* Disconnection Complete: status, handle, reason */
    if (n < 4 || p[0] || !conn || ((p[1] | p[2] << 8u) & 0xFFF) != handle) return;
    conn = 0; aqh = aqt; pend_op = 0;
    if (held) { for (unsigned b = 0; b < 3; b++) if (held >> b & 1) kinput(1, 272 + b, 0); held = 0; kinput(0, 0, 0); }
    if (st == PAIRING && closing) { st = READY; closing = 0; until = FOREVER; return; }   /* the line was printed by fail */
    unsigned lived = (unsigned)((now() - conn_t0) / ms(1));   /* how long the link was up, in ms */
    if (st == PAIRING && p[3] == 0x3E && smp < 4 && tries < 3) {   /* connection failed to be established: the mouse never answered, call again, at another speed */
        tries++; smp = gs = 0;
        put("bt: the mouse never answered ("); put_dec((int)lived); put(" ms), calling again"); say();
        pc = 33; until = 0; return;   /* from the address step, so the Create Connection is built fresh */
    }
    if (st == PAIRING) { put(smp < 4 ? "bt: pairing failed, the mouse hung up after " : "bt: the mouse hung up after "); put_dec((int)lived); put(" ms, reason "); put_hex(p[3]); fail(0); return; }
    if (st == MOUSE) { put("bt: the mouse disconnected, reason "); put_hex(p[3]); say(); st = READY; }
}

/* ---- H4 in: a packet-type byte, a header, the body. Anything else is skipped a byte at a time until it lines up. ---- */
static unsigned char rx[260]; static unsigned rxn, rxneed, rxtype, rxhdr;
static void on_event(unsigned code, const unsigned char *p, unsigned n) {
    if (code == 0x0E && n >= 3 && want && (unsigned)(p[1] | p[2] << 8) == want) {        /* Command Complete: count, opcode, returns */
        want = 0;
        if (!by_step) return;                    /* a link command (LE Connection Update, Disconnect): nothing waits on it */
        retn = n - 3 < sizeof ret ? n - 3 : sizeof ret;
        for (unsigned i = 0; i < retn; i++) ret[i] = p[3 + i];
        until = 0;                               /* answered: the next step may run at once */
    } else if (code == 0x0F && n >= 4 && want && (unsigned)(p[2] | p[3] << 8) == want) { /* Command Status: status, count, opcode */
        want = 0;
        if (!by_step) return;
        ret[0] = p[0]; retn = 1; until = 0;
    } else if (code == 0x3E && n >= 2 && p[0] == 0x02 && st == SCANNING) adv_reports(p + 1, n - 1);
    else if (code == 0x3E && n >= 2 && p[0] == 0x01) connected(p + 1, n - 1);
    else if (code == 0x13 && n >= 1) {            /* Number of Completed Packets: handles, then counts */
        for (unsigned k = 0, m = p[0]; k < m && 1 + 4 * m <= n; k++)
            if (((p[1 + 2 * k] | p[2 + 2 * k] << 8u) & 0xFFF) == handle) credits += p[1 + 2 * m + 2 * k] | p[2 + 2 * m + 2 * k] << 8u;
        acl_flush();
    } else if (code == 0x08 && n >= 4 && conn && ((p[1] | p[2] << 8u) & 0xFFF) == handle && st == PAIRING && (pc == 43 || pc == 44)) {   /* Encryption Change */
        if (p[0] || !p[3]) { put("bt: pairing failed, encryption did not come on, status "); put_hex(p[0]); fail(0); return; }
        put("bt: paired"); say();
        pc = 50; until = 0;
    } else if (code == 0x05) disconnected(p, n);
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
        if (rxtype == 4) on_event(rx[0], rx + 2, rx[1]);
        else if (rxn <= sizeof rx) on_acl(rx, rxn); else acl_in++;
        rxtype = 0;
    }
}

/* ---- The steps. Each runs once the previous command has its answer and any pause is over; a wait on the mouse is
   a step whose time is its deadline, and the event that ends the wait moves pc past it. ---- */
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
        ln = 0; put("bt: the radio refused "); put_hex(last_op); put(", status "); put_hex(ret[0]);
        retn = 0;
        if (st == PAIRING) { fail(0); return; }
        say();
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
    case 13:
        st = READY;
        if (scan_later) {   /* a scan was asked for during start-up: run it now, into the Terminal that asked */
            scan_later = 0; put("bt: ready, scanning for 5 seconds"); say();
            ndev = nlost = nshown = 0; to_term = 1; st = SCANNING; pc = 20; until = now(); return;
        }
        put("bt: ready, type bt scan to look for devices"); say(); return;
    case 20: cmd(OP_SCAN_PARAMS, scan, 7); return;
    case 21: cmd(OP_SCAN_ENABLE, on, 2); return;
    case 22: pause_ms(SCAN_MS); return;
    case 23: cmd(OP_SCAN_ENABLE, off, 2); return;
    case 24: {   /* the list, in words: the interesting ones numbered, strongest group first; the nameless rest counted */
        unsigned ord[DEVS], n = 0;
        for (unsigned k = 0; k < ndev; k++) if (rank(k) >= 1000) ord[n++] = k;
        for (unsigned x = 1; x < n; x++) { unsigned v = ord[x]; int y = (int)x - 1; while (y >= 0 && rank(ord[y]) < rank(v)) { ord[y + 1] = ord[y]; y--; } ord[y + 1] = v; }
        nshown = n > 9 ? 9 : n;
        for (unsigned r = 0; r < nshown; r++) {
            unsigned k = ord[r]; shown[r] = k;
            const char *w = kind_word(k);
            put("bt: "); put_dec((int)r + 1); put(". ");
            if (dev[k].name[0]) { put(dev[k].name); unsigned wl = 0; while (w && w[wl]) wl++; if (w && dev[k].hid && !has(dev[k].name, w, wl)) { put(" ("); put(w); put(")"); } }
            else put(w ? w : "Unknown device");
            put(", "); put(near_word(dev[k].rssi));
            if (dev[k].hid) {   /* how it advertises: only a connectable one can be called */
                put(dev[k].evts & 1 ? " [connectable" : dev[k].evts & 2 ? " [directed to another device, not to us" : dev[k].evts & 12 ? " [NOT connectable" : " [?");
                put(dev[k].adfl & 3 ? ", discoverable]" : dev[k].adfl ? ", not in pairing mode]" : "]");
            }
            if (dev[k].hid == 2 || (dev[k].hid && r == 0)) put("   <- your mouse? bt pair");
            say();
        }
        unsigned rest = ndev - nshown + nlost;
        if (!ndev) put("bt: nothing heard. Is the radio on? Try bt scan again");
        else { put("bt: "); put_dec((int)ndev + (int)nlost); put(" devices heard"); if (rest) { put(", "); put_dec((int)rest); put(" more with no name or type, hidden"); } }
        say();
        if (!nshown || !(dev[shown[0]].hid)) { put("bt: no mouse yet. Hold your mouse's pairing button until its light blinks, put it next to the Pi, and bt scan again"); say(); }
        else { put("bt: bt pair connects to number 1, or bt pair 2 for another"); say(); }
        st = READY; to_term = 0; return;
    }
    /* `bt pair`: buffers, our address, the connection */
    case 30: cmd(OP_LE_BUFSIZE, 0, 0); return;
    case 31:   /* status, LE ACL length (2), LE ACL packets; 0 packets means the radio shares its classic buffers */
        if (ret[3]) { credits = ret[3]; pc = 33; step(); return; }
        cmd(OP_BUFSIZE, 0, 0); return;
    case 32: credits = ret[4] | ret[5] << 8u;   /* status, ACL length (2), SCO length, ACL packets (2), SCO packets (2) */
        /* fall through */
    case 33:
        if (!credits) { fail("bt: the radio has no room for data"); return; }
        pc = 34; cmd(OP_BDADDR, 0, 0); return;
    case 34: {
        for (int i = 0; i < 6; i++) me[i] = ret[1 + i];
        /* scan interval 60 ms and window 30 ms, no filter, the mouse, our public address, interval 30 to 50 ms (Linux's
           defaults; on the Pi 4 the radio shares its antenna with Wi-Fi, and at 7.5 ms the first packets to a real mouse
           never got through: the link dropped with 0x3e, connection failed to be established), no latency, supervision
           timeout 2 s, no connection event lengths */
        unsigned char c[25] = { 0x60, 0, 0x30, 0, 0, peer_type, peer[0], peer[1], peer[2], peer[3], peer[4], peer[5], 0,
                                24, 0, 40, 0, 0, 0, 200, 0, 0, 0, 0, 0 };
        if (tries == 1) { c[13] = 12; c[15] = 24; }   /* the second call: 15 to 30 ms */
        if (tries >= 2) { c[13] = 6; c[15] = 12; }    /* the third: 7.5 to 15 ms */
        cmd(OP_CONNECT, c, 25); return;
    }
    case 35: until = now() + ms(LINK_MS); return;   /* Command Status came back: now wait for the link */
    case 36:   /* no LE Connection Complete in time: the mouse is not advertising */
        put("bt: the mouse did not answer, hold its button 5 seconds and type bt pair again"); say(); to_term = 0;
        cmd(OP_CONNECT_CANCEL, 0, 0); return;
    case 37: st = READY; until = FOREVER; wifi_release(); return;
    case 40: {   /* SMP Pairing Request: no input or output, no OOB, bonding, a 16 byte key; the mouse sends its LTK */
        static const unsigned char rq[7] = { 0x01, 0x03, 0x00, 0x01, 16, 0x00, 0x01 };
        for (int i = 0; i < 7; i++) preq[i] = rq[i];
        smp = 1; smp_send(rq, 7); return;
    }
    case 41: fail("bt: pairing failed, the mouse stopped answering"); return;
    case 42: {   /* LE Start Encryption with the STK: Rand and EDIV are zero for an STK */
        unsigned char e[28] = { (unsigned char)handle, (unsigned char)(handle >> 8) };
        for (int i = 0; i < 16; i++) e[12 + i] = stk[i];
        cmd(OP_ENCRYPT, e, 28); wipe(stk, 16); wipe(e, 28); return;
    }
    case 43: until = now() + ms(LINK_MS); return;   /* Command Status: now wait for Encryption Change */
    case 44: fail("bt: pairing failed, encryption did not come on"); return;
    case 50: {   /* Find By Type Value: primary services (0x2800) whose UUID is HID (0x1812) */
        static const unsigned char q[9] = { 0x06, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x28, 0x12, 0x18 };
        gs = 1; att_req(q, 9); return;
    }
    case 51: fail("bt: the mouse stopped answering"); return;
    case 60: conn = 0; closing = 0; st = READY; until = FOREVER; return;   /* the hang-up never came back: let it go */
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
    if (st != STARTING && st != SCANNING && st != PAIRING && st != MOUSE) return;
    acl_flush();
    for (unsigned k = 0; k < 512 && !(U(FR) & RXFE); k++) {
        unsigned d = U(DR);
        if (d & 0xF00) rx_err++;          /* framing, parity, break or overrun */
        rx_byte((unsigned char)d);
        if (st == FAILED) return;
    }
    if (want) { if (now() > cmd_until) stop("bt: no answer from the radio"); return; }
    if (pend_op && (pend_op != OP_DISCONNECT || aqh == aqt)) {   /* a hang-up waits for what is queued before it */
        unsigned op = pend_op; pend_op = 0;
        send_cmd(op, pend_p, pend_n, 0);
        return;
    }
    if (now() < until) return;
    step();
}

int bt_busy(void) { return st == STARTING || st == SCANNING || st == PAIRING || st == MOUSE; }

static int has(const char *name, const char *w, unsigned wn) {   /* name contains the wn letters of w, any case */
    for (unsigned i = 0; name[i]; i++) {
        unsigned j = 0;
        while (j < wn && name[i + j] && (name[i + j] | 32) == (w[j] | 32)) j++;
        if (j == wn) return 1;
    }
    return 0;
}

int bt_command(const char *q, unsigned n) {
    if (n < 2 || q[0] != 'b' || q[1] != 't' || (n > 2 && q[2] != ' ')) return 0;
    unsigned i = 2; while (i < n && q[i] == ' ') i++;
    int scan = n - i == 4 && q[i] == 's' && q[i + 1] == 'c' && q[i + 2] == 'a' && q[i + 3] == 'n';
    int pair = n - i >= 4 && q[i] == 'p' && q[i + 1] == 'a' && q[i + 2] == 'i' && q[i + 3] == 'r' && (n - i == 4 || q[i + 4] == ' ');
    if (i < n && !scan && !pair) { put("bt: try bt, bt scan or bt pair"); say_now(); return 1; }
    if (scan && st == STARTING) {   /* the radio is still loading its patch (about half a minute after power-on): wait for it, then scan */
        scan_later = 1; to_term = 1;
        put("bt: the radio is still starting"); if (pc == 6 && bt_fw_hcd_len) { put(" ("); put_dec((int)(fwpos * 100u / bt_fw_hcd_len)); put("% of its patch loaded)"); }
        put(". I will scan the moment it is ready"); say_now(); return 1;
    }
    if (scan && st == READY) {
        ndev = nlost = nshown = 0; to_term = 1; st = SCANNING; pc = 20; until = now();
        put("bt: scanning for 5 seconds"); say_now(); return 1;
    }
    if (pair && st == READY) {   /* `bt pair [NAME]`: NAME, else the strongest device that says it is a mouse, then one that says HID, then the first name with "mouse" in it, else the strongest */
        unsigned w = i + 4; while (w < n && q[w] == ' ') w++;
        int k = -1;
        if (w < n && q[w] >= '1' && q[w] <= '9' && w + 1 == n) {   /* bt pair 2: the second row of the last list */
            unsigned r = (unsigned)(q[w] - '1');
            if (r >= nshown) { put("bt: no number "); put_dec((int)r + 1); put(" in the list, type bt scan first"); say_now(); return 1; }
            k = (int)shown[r];
        }
        for (unsigned d = 0; d < ndev && k < 0; d++) if (w < n ? has(dev[d].name, q + w, n - w) : has(dev[d].name, "mouse", 5)) k = (int)d;
        for (int want_hid = 2; want_hid >= 1 && k < 0 && w == n; want_hid--)   /* a device that advertised itself as a mouse, else as HID: the strongest of those */
            for (unsigned d = 0; d < ndev; d++) if (dev[d].hid >= want_hid && (k < 0 || (dev[d].rssi != 127 && (dev[k].rssi == 127 || dev[d].rssi > dev[k].rssi)))) k = (int)d;
        if (k < 0 && w == n && !nshown) { put("bt: type bt scan first, so there is a list to pair from"); say_now(); return 1; }
        if (k < 0 && w == n)   /* no name with "mouse" in it: the strongest signal, an unmeasured one last */
            for (unsigned d = 0; d < ndev; d++) if (k < 0 || (dev[d].rssi != 127 && (dev[k].rssi == 127 || dev[d].rssi > dev[k].rssi))) k = (int)d;
        if (k < 0) { put(ndev ? "bt: nothing heard by that name, type bt scan" : "bt: nothing heard yet, type bt scan first"); say_now(); return 1; }
        for (int j = 0; j < 6; j++) peer[j] = dev[k].a[j];
        peer_type = dev[k].type;
        put("bt: dialling "); for (int j = 5; j >= 0; j--) { put_hex(peer[j]); if (j) put(":"); } put(peer_type ? " (random address)" : " (public address)"); say();
        unsigned m = 0; for (; dev[k].name[m] && m < sizeof peer_name - 1; m++) peer_name[m] = dev[k].name[m]; peer_name[m] = 0;
        conn = closing = smp = gs = tries = 0; nsvc = cccd = pm_vh = boot_vh = next_decl = held = credits = keys = 0; aqh = aqt = 0;
        wipe(ltk, 16); wipe(krand, 8);
        wifi_hold();
        to_term = 1; st = PAIRING; pc = 30; until = now();
        put("bt: connecting to "); put(peer_name[0] ? peer_name : kind_word((unsigned)k) ? kind_word((unsigned)k) : "the strongest device"); put(", keep it in pairing mode"); say_now(); return 1;
    }
    switch (st) {
    case READY: put("bt: ready, version "); put(version); break;
    case SCANNING: put("bt: scanning"); break;
    case PAIRING: put("bt: pairing with "); put(peer_name[0] ? peer_name : "the mouse"); break;
    case MOUSE: put("bt: mouse ready, "); put(peer_name[0] ? peer_name : "no name"); break;
    case STARTING:
        if (pc == 6 && nrec && bt_fw_hcd_len) { put("bt: loading the radio's patch, "); put_dec((int)(fwpos * 100u / bt_fw_hcd_len)); put("% done"); }
        else put("bt: starting");
        break;
    default: put(why); break;
    }
    say_now();
    if ((st == READY || st == MOUSE) && (acl_in || rx_err || rx_junk)) {
        put("bt: acl in "); put_dec((int)acl_in); put(", rx errors "); put_dec((int)rx_err); put(", stray bytes "); put_dec((int)rx_junk); say_now();
    }
    return 1;
}
