/* Classic Bluetooth (12.1, BR/EDR), beside bt.c's LE mouse: headphones, speakers and keyboards are classic devices.
   It shares bt.c's UART, H4 framing and single command slot: bt.c hands every event here (btc_event), every answer to a
   command it did not send for a step (btc_done), and asks btc_poll to send the next queued command only when no command is
   in flight. Opcodes and event layouts are Linux's include/net/bluetooth/hci.h.

   `bt find`: the first time, Set Event Mask again with the classic pairing events on (bt.c's mask plus 0x29-0x36, LE Meta
   kept), Write Simple Pairing Mode 1 and Write Inquiry Mode 2 (results with RSSI and the extended inquiry data that carries
   the name), so bring-up and the LE mouse stay byte for byte as they were. Then Inquiry on the general access code for
   6 x 1.28 s. Each device heard once, its name from the extended data (0x09, else 0x08), what it is from its Class of
   Device (headphones, speaker, keyboard, mouse), how close from its RSSI; Inquiry Complete prints the numbered list.

   `bt connect N` (or `bt connect` for the saved one): Create Connection, Connection Complete, Authentication Requested.
   The radio then asks: Link Key Request (the saved key if this is the saved address, else the negative reply), IO
   Capability Request (we answer NoInputNoOutput, no MITM, general bonding: Secure Simple Pairing's Just Works, which is
   what headphones and speakers do), User Confirmation Request (yes), PIN Code Request (an old device: 0000). Link Key
   Notification is saved to JT/BTLINK.BIN at once: "JTL1", address, key, key type, Class of Device, name. Authentication
   Complete, Set Connection Encryption, Encryption Change: `bt: NAME connected`. A saved key the device no longer knows
   (Authentication Complete 0x05 or 0x06) is erased. Every wait has a deadline on the generic timer; no key is ever printed.
   `bt disconnect` hangs up. 12.2 adds the HID channels on this link, 13.0 audio. */
#include "btc.h"
#include "emmc.h"
#include "../../drivers/fat.h"

void kputs(const char *s);
extern void (*cmd_out)(const char *s);
void term_output(int on); void ask_redraw(void);
void bt_hci_cmd(unsigned op, const unsigned char *p, unsigned n);   /* bt.c: one command, not a step */
int bt_radio_ready(void);                                           /* bt.c: bring-up is done */

#define LINK_FILE "BTLINK.BIN"
#define LINK_SIZE 62                     /* "JTL1", address 6, key 16, key type 1, class 3, name 32 */
#define FIND_MS 10000                    /* Inquiry is 7.68 s; Inquiry Complete comes by then */
#define PAGE_MS 15000                    /* Create Connection: the device must be on and near */
#define PAIR_MS 30000

enum { IDLE, FINDING, CONNECTING, PAIRING, ENCRYPTING, UP };
static int st, setup, term;
static unsigned long until;
static unsigned sent;                    /* the opcode of ours in flight, 0 when none */
static unsigned handle;
static unsigned char peer[6], peer_cod[3], peer_psrm; static char peer_name[32];
static unsigned char saved_addr[6]; static int has_saved;
static void hid_open(unsigned c); static void let_go(void); void btc_flush(void);
static unsigned credits, hs;   /* 12.2: the radio's classic ACL buffers free; the HID channels' stage */
#define AQ 6
static unsigned char aq[AQ][40], aqn[AQ]; static unsigned aqh, aqt;

static unsigned long now(void) { unsigned long t; __asm__ volatile ("isb\n mrs %0, cntpct_el0" : "=r"(t)); return t; }
static unsigned long ms(unsigned n) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return (f ? f : 54000000) / 1000 * n; }
static void wipe(void *p, unsigned n) { for (unsigned i = 0; i < n; i++) ((volatile unsigned char *)p)[i] = 0; }
static int same6(const unsigned char *a, const unsigned char *b) { for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0; return 1; }

static char line[112]; static unsigned ln;
static void put(const char *s) { while (*s && ln < sizeof line - 2) line[ln++] = *s++; }
static void put_dec(int v) { char t[12]; unsigned n = 11, u = v < 0 ? (unsigned)-v : (unsigned)v; t[n] = 0;
    do t[--n] = (char)('0' + u % 10); while (u /= 10); if (v < 0) t[--n] = '-'; put(t + n); }
static void put_hex(unsigned v) { char t[11]; unsigned n = 10; t[n] = 0; do t[--n] = "0123456789abcdef"[v & 15]; while (v >>= 4); put("0x"); put(t + n); }
static void say(void) { line[ln++] = '\n'; line[ln] = 0; ln = 0; if (term) term_output(1); kputs(line); if (term) { term_output(0); ask_redraw(); } }
static void say_now(void) { line[ln++] = '\n'; line[ln] = 0; ln = 0; cmd_out(line); }

/* ---- Commands wait here for bt.c's slot. ---- */
#define CQ 6
static unsigned short cq_op[CQ]; static unsigned char cq_p[CQ][24], cq_n[CQ]; static unsigned cqh, cqt;
static void queue(unsigned op, const unsigned char *p, unsigned n) {
    if (cqt - cqh == CQ || n > 24) return;           /* full: the step waiting on it times out with a plain line */
    unsigned k = cqt++ % CQ; cq_op[k] = (unsigned short)op; cq_n[k] = (unsigned char)n;
    for (unsigned i = 0; i < n; i++) cq_p[k][i] = p[i];
}
static void q_addr(unsigned op, const unsigned char *a, const unsigned char *rest, unsigned rn) {   /* address, then rest */
    unsigned char b[24]; for (int i = 0; i < 6; i++) b[i] = a[i];
    for (unsigned i = 0; i < rn; i++) b[6 + i] = rest[i];
    queue(op, b, 6 + rn); wipe(b, sizeof b);
}
static void q_handle(unsigned op, unsigned extra, int with) {
    unsigned char b[3] = { (unsigned char)handle, (unsigned char)(handle >> 8), (unsigned char)extra };
    queue(op, b, with ? 3 : 2);
}

/* ---- The saved device ---- */
static int link_load(unsigned char *b) {   /* the record into b (LINK_SIZE), 1 when whole */
    if (!sd_ready() || !sd_enter_jt(0)) return 0;
    int got = fat_read_file(LINK_FILE, b, LINK_SIZE);
    fat_chdir("..");
    return got == LINK_SIZE && b[0] == 'J' && b[1] == 'T' && b[2] == 'L' && b[3] == '1';
}
static void link_check(void) {
    unsigned char b[LINK_SIZE]; has_saved = link_load(b);
    if (has_saved) for (int i = 0; i < 6; i++) saved_addr[i] = b[4 + i];
    wipe(b, sizeof b);
}
static void link_save(const unsigned char *key, unsigned type) {
    unsigned char b[LINK_SIZE];
    wipe(b, sizeof b);
    b[0] = 'J'; b[1] = 'T'; b[2] = 'L'; b[3] = '1';
    for (int i = 0; i < 6; i++) b[4 + i] = peer[i];
    for (int i = 0; i < 16; i++) b[10 + i] = key[i];
    b[26] = (unsigned char)type;
    for (int i = 0; i < 3; i++) b[27 + i] = peer_cod[i];
    for (int i = 0; i < 31 && peer_name[i]; i++) b[30 + i] = (unsigned char)peer_name[i];
    int ok = sd_ready() && sd_write(LINK_FILE, b, LINK_SIZE) == 1;
    wipe(b, sizeof b);
    put(ok ? "bt: pairing saved" : "bt: pairing not saved, no card to keep it on"); say();
    link_check();
}

/* ---- What inquiry heard ---- */
#define CDEVS 9
static struct { unsigned char a[6], cod[3], psrm; signed char rssi; char name[32]; } dev[CDEVS];
static unsigned ndev;
static const char *kind(const unsigned char *cod) {   /* Class of Device: major class bits 8-12, minor 2-7 */
    unsigned c = cod[0] | cod[1] << 8u | (unsigned)cod[2] << 16u, major = c >> 8 & 31, minor = c >> 2 & 63;
    if (major == 4) return minor == 5 ? "speaker" : minor == 1 || minor == 2 || minor == 6 ? "headphones" : "audio device";
    if (major == 5) { unsigned k = minor >> 4 & 3; return k == 1 ? "keyboard" : k == 2 ? "mouse" : k == 3 ? "keyboard and mouse" : "input device"; }
    if (major == 2) return "phone";
    if (major == 1) return "computer";
    return "device";
}
static const char *near(int rssi) { return rssi == 127 ? "signal unknown" : rssi >= -45 ? "very close" : rssi >= -60 ? "near" : rssi >= -75 ? "in the room" : "far away"; }
static void heard(const unsigned char *a, unsigned psrm, const unsigned char *cod, int rssi, const unsigned char *eir, unsigned en) {
    unsigned k = 0;
    while (k < ndev && !same6(dev[k].a, a)) k++;
    if (k == ndev) { if (ndev == CDEVS) return; ndev++; for (int i = 0; i < 6; i++) dev[k].a[i] = a[i]; dev[k].name[0] = 0; }
    for (int i = 0; i < 3; i++) dev[k].cod[i] = cod[i];
    dev[k].psrm = (unsigned char)psrm; dev[k].rssi = (signed char)rssi;
    for (unsigned i = 0; i + 1 < en && eir[i] && i + 1 + eir[i] <= en; i += 1 + eir[i]) {   /* length, type, data */
        unsigned t = eir[i + 1], len = eir[i] - 1u;
        if (t != 0x09 && !(t == 0x08 && !dev[k].name[0])) continue;
        unsigned m = 0;
        for (; m < len && m < sizeof dev[k].name - 1; m++) { unsigned char c = eir[i + 2 + m]; dev[k].name[m] = c >= 32 && c < 127 ? (char)c : '?'; }
        dev[k].name[m] = 0;
    }
}
static void list(void) {
    for (unsigned k = 0; k < ndev; k++) {
        const char *w = kind(dev[k].cod);
        put("bt: "); put_dec((int)k + 1); put(". ");
        if (dev[k].name[0]) { put(dev[k].name); put(", "); put(w); } else { put("Unnamed "); put(w); }
        put(", "); put(near(dev[k].rssi)); say();
    }
    put(ndev ? "bt: bt connect 1 pairs number 1. Put headphones in pairing mode first" : "bt: no classic devices heard. Put the headphones in pairing mode and bt find again");
    say();
}

static void fail(const char *msg, int code) {
    put(msg); if (code >= 0) { put(", status "); put_hex((unsigned)code); } say();
    if (st >= PAIRING || (st == CONNECTING && handle)) q_handle(0x0406, 0x13, 1);   /* Disconnect: we ended it */
    if (hs) { let_go(); hs = 0; }
    st = IDLE; until = 0; term = 0;
}

int btc_command(const char *q, unsigned n) {   /* q is what follows "bt " */
    int find = n == 4 && q[0] == 'f' && q[1] == 'i' && q[2] == 'n' && q[3] == 'd';
    int conn = n >= 7 && q[0] == 'c' && q[1] == 'o' && q[2] == 'n' && q[3] == 'n' && q[4] == 'e' && q[5] == 'c' && q[6] == 't' && (n == 7 || q[7] == ' ');
    int disc = n == 10 && q[0] == 'd' && q[1] == 'i' && q[2] == 's' && q[3] == 'c' && q[4] == 'o' && q[5] == 'n';
    if (!find && !conn && !disc) return 0;
    if (!bt_radio_ready()) { put("bt: the radio is not ready yet"); say_now(); return 1; }
    if (disc) {
        if (st != UP) { put("bt: no classic device connected"); say_now(); return 1; }
        q_handle(0x0406, 0x13, 1); put("bt: hanging up "); put(peer_name[0] ? peer_name : "the device"); say_now(); return 1;
    }
    if (st != IDLE) { put("bt: busy, one thing at a time"); say_now(); return 1; }
    if (!setup) {   /* the classic events, Simple Pairing, extended inquiry results */
        static const unsigned char mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F, 0x20 }, one[1] = { 1 }, two[1] = { 2 };
        queue(0x0C01, mask, 8); queue(0x0C56, one, 1); queue(0x0C45, two, 1); queue(0x1005, 0, 0); setup = 1;
    }
    term = 1;
    if (find) {
        static const unsigned char inq[5] = { 0x33, 0x8B, 0x9E, 6, 0 };   /* GIAC, 6 x 1.28 s, no limit */
        ndev = 0; queue(0x0401, inq, 5); st = FINDING; until = now() + ms(FIND_MS);
        put("bt: looking for headphones, speakers and keyboards for 8 seconds"); say_now(); return 1;
    }
    unsigned i = 7; while (i < n && q[i] == ' ') i++;
    if (i < n) {
        if (q[i] < '1' || q[i] > '9' || i + 1 != n || (unsigned)(q[i] - '1') >= ndev) { put("bt: no such number, bt find first"); say_now(); term = 0; return 1; }
        unsigned k = (unsigned)(q[i] - '1');
        for (int j = 0; j < 6; j++) peer[j] = dev[k].a[j];
        for (int j = 0; j < 3; j++) peer_cod[j] = dev[k].cod[j];
        peer_psrm = dev[k].psrm;
        unsigned m = 0; for (; dev[k].name[m] && m < 31; m++) peer_name[m] = dev[k].name[m]; peer_name[m] = 0;
    } else {
        unsigned char b[LINK_SIZE];
        if (!link_load(b)) { wipe(b, sizeof b); put("bt: nothing saved, bt find then bt connect N"); say_now(); term = 0; return 1; }
        for (int j = 0; j < 6; j++) peer[j] = b[4 + j];
        for (int j = 0; j < 3; j++) peer_cod[j] = b[27 + j];
        unsigned m = 0; for (; b[30 + m] && m < 31; m++) peer_name[m] = (char)b[30 + m]; peer_name[m] = 0;
        peer_psrm = 1; wipe(b, sizeof b);
    }
    link_check();
    /* Create Connection: DM1-DH5 packets (0xCC18, as Linux), its page scan mode, no clock offset, a role switch allowed */
    unsigned char rest[7] = { 0x18, 0xCC, peer_psrm, 0, 0, 0, 1 };
    q_addr(0x0405, peer, rest, 7); handle = 0;
    st = CONNECTING; until = now() + ms(PAGE_MS);
    put("bt: connecting to "); put(peer_name[0] ? peer_name : kind(peer_cod)); say_now();
    return 1;
}

void btc_done(unsigned op, const unsigned char *r, unsigned n) {   /* Command Complete or Command Status for a command of ours */
    if (!sent || op != sent) return;
    sent = 0;
    if (op == 0x1005 && n >= 6 && !r[0]) { credits = r[4] | r[5] << 8u; return; }   /* status, ACL length, SCO length, ACL packets */
    if (!n || !r[0]) return;
    if (op == 0x0401) { put("bt: the radio would not look, status "); put_hex(r[0]); say(); st = IDLE; term = 0; return; }
    if (op == 0x0405 || op == 0x0411 || op == 0x0413) fail("bt: the radio refused to connect", r[0]);
}

void btc_event(unsigned code, const unsigned char *p, unsigned n) {
    if (code == 0x13 && n >= 1) {   /* Number of Completed Packets: handles, then counts */
        for (unsigned k = 0, m = p[0]; k < m && 1 + 4 * m <= n; k++)
            if (handle && ((p[1 + 2 * k] | p[2 + 2 * k] << 8u) & 0xFFF) == handle) credits += p[1 + 2 * m + 2 * k] | p[2 + 2 * m + 2 * k] << 8u;
        btc_flush(); return;
    }
    if (code == 0x2F && n >= 255 && st == FINDING && p[0] == 1) heard(p + 1, p[7], p + 9, (signed char)p[14], p + 15, 240);   /* one result, its 240 bytes of data */
    else if (code == 0x22 && n >= 1 && st == FINDING) for (unsigned k = 0; k < p[0] && 1 + 14 * (k + 1) <= n; k++) { const unsigned char *r = p + 1 + 14 * k; heard(r, r[6], r + 8, (signed char)r[13], 0, 0); }
    else if (code == 0x02 && n >= 1 && st == FINDING) for (unsigned k = 0; k < p[0] && 1 + 14 * (k + 1) <= n; k++) { const unsigned char *r = p + 1 + 14 * k; heard(r, r[6], r + 9, 127, 0, 0); }
    else if (code == 0x01 && st == FINDING) { list(); st = IDLE; term = 0; }
    else if (code == 0x03 && n >= 11 && st == CONNECTING && same6(p + 3, peer)) {   /* Connection Complete */
        if (p[0]) { fail("bt: could not connect, is it on and in pairing mode?", p[0]); return; }
        handle = (p[1] | p[2] << 8u) & 0xFFF;
        put("bt: link up, pairing"); say();
        q_handle(0x0411, 0, 0); st = PAIRING; until = now() + ms(PAIR_MS);   /* Authentication Requested */
    }
    else if (st < CONNECTING || st > ENCRYPTING) { if (code == 0x05 && st == UP && n >= 4 && ((p[1] | p[2] << 8u) & 0xFFF) == handle) { put("bt: "); put(peer_name[0] ? peer_name : "the device"); put(" disconnected, reason "); put_hex(p[3]); say(); st = IDLE; if (hs) let_go(); hs = 0; aqh = aqt; } }
    else if (code == 0x17 && n >= 6 && same6(p, peer)) {   /* Link Key Request: ours, if this is the saved device */
        unsigned char b[LINK_SIZE];
        if (link_load(b) && same6(b + 4, peer)) q_addr(0x040B, peer, b + 10, 16);
        else q_addr(0x040C, peer, 0, 0);
        wipe(b, sizeof b);
    }
    else if (code == 0x31 && n >= 6 && same6(p, peer)) { static const unsigned char io[3] = { 0x03, 0x00, 0x04 }; q_addr(0x042B, peer, io, 3); }   /* NoInputNoOutput, no OOB, general bonding */
    else if (code == 0x33 && n >= 6 && same6(p, peer)) q_addr(0x042C, peer, 0, 0);   /* Just Works: yes */
    else if (code == 0x16 && n >= 6 && same6(p, peer)) { unsigned char pin[17] = { 4, '0', '0', '0', '0' }; q_addr(0x040D, peer, pin, 17); }
    else if (code == 0x36 && n >= 7 && p[0] && same6(p + 1, peer)) fail("bt: pairing failed", p[0]);
    else if (code == 0x18 && n >= 23 && same6(p, peer)) link_save(p + 6, p[22]);
    else if (code == 0x06 && n >= 3 && ((p[1] | p[2] << 8u) & 0xFFF) == handle && st == PAIRING) {   /* Authentication Complete */
        if (p[0] == 0x05 || p[0] == 0x06) {
            if (has_saved && same6(saved_addr, peer)) { sd_remove(LINK_FILE); has_saved = 0; put("bt: the device no longer knows us, its pairing is forgotten. Pairing mode, then bt find"); say(); }
            fail("bt: pairing failed", p[0]); return;
        }
        if (p[0]) { fail("bt: pairing failed", p[0]); return; }
        q_handle(0x0413, 1, 1); st = ENCRYPTING; until = now() + ms(PAIR_MS);   /* Set Connection Encryption: on */
    }
    else if (code == 0x08 && n >= 4 && ((p[1] | p[2] << 8u) & 0xFFF) == handle && st == ENCRYPTING) {
        if (p[0] || !p[3]) { fail("bt: encryption did not come on", p[0]); return; }
        put("bt: "); put(peer_name[0] ? peer_name : kind(peer_cod)); put(" connected"); say();
        st = UP; until = 0; term = 0;
        if ((peer_cod[1] & 31) == 5) { term = 1; hid_open(0); until = now() + ms(10000); }   /* a keyboard or mouse: its HID channels */
    }
    else if (code == 0x05 && n >= 4 && ((p[1] | p[2] << 8u) & 0xFFF) == handle) { put("bt: the device hung up, reason "); put_hex(p[3]); say(); st = IDLE; until = 0; term = 0; if (hs) let_go(); hs = 0; aqh = aqt; }
}

/* ---- 12.2: a keyboard or mouse, HID over L2CAP on this link. Once encrypted, a peripheral (major class 5) gets two
   channels, as Linux's hidp opens them: control on PSM 0x11, then interrupt on 0x13, each a Connection Request on the
   signalling channel (CID 1), then a Configuration Request each way (we ask for an MTU of 672 and accept theirs). Then
   SET_PROTOCOL to boot (0x70) on control, so reports have the boot layout: on interrupt, DATA Input (0xA1), report ID 1 a
   keyboard's 8 bytes (to xhci.c's hid_kbd, as a USB keyboard's are), ID 2 a mouse's buttons, x, y and wheel (to kinput).
   Information Requests get "not supported", a Disconnection Request its response, anything else a Command Reject. ACL
   packets out spend the radio's classic buffers (Read Buffer Size) and Number of Completed Packets gives them back.
   ponytail: bt.c's LE mouse counts the same shared buffers on its own; a mouse and a keyboard both busy could overfill
   them by a packet or two, add one shared count if the board ever shows it. ---- */
void hid_kbd(const unsigned char *r, unsigned len, unsigned char *p);   /* xhci.c */
void kinput(unsigned type, unsigned code, int value);                   /* main.c */
int bt_tx(const unsigned char *b, unsigned n);                          /* bt.c: one H4 packet into the UART queue */
static unsigned sig_id, held;   /* hs: 0 none, 1 control opening, 2 interrupt opening, 3 ready */
static unsigned lcid[2] = { 0x0040, 0x0041 }, rcid[2], cfg_in[2], cfg_out[2];
static unsigned char kb_prev[8];
void btc_flush(void) {
    while (aqh != aqt && credits && bt_tx(aq[aqh % AQ], aqn[aqh % AQ])) { wipe(aq[aqh % AQ], 40); aqh++; credits--; }
}
static void l2_send(unsigned cid, const unsigned char *p, unsigned n) {   /* one L2CAP frame, one ACL packet (PB 10: a start) */
    if (n > 31 || aqt - aqh == AQ) return;
    unsigned char *b = aq[aqt % AQ];
    b[0] = 2; b[1] = (unsigned char)handle; b[2] = (unsigned char)((handle >> 8 & 0x0F) | 0x20);
    b[3] = (unsigned char)(n + 4); b[4] = 0; b[5] = (unsigned char)n; b[6] = 0; b[7] = (unsigned char)cid; b[8] = (unsigned char)(cid >> 8);
    for (unsigned i = 0; i < n; i++) b[9 + i] = p[i];
    aqn[aqt % AQ] = (unsigned char)(9 + n); aqt++;
    btc_flush();
}
static void sig(unsigned code, unsigned id, const unsigned char *d, unsigned n) {
    unsigned char b[16] = { (unsigned char)code, (unsigned char)id, (unsigned char)n, 0 };
    for (unsigned i = 0; i < n && i < 12; i++) b[4 + i] = d[i];
    l2_send(1, b, 4 + n);
}
static void hid_open(unsigned c) {   /* Connection Request: PSM, our CID */
    unsigned psm = c ? 0x13 : 0x11;
    unsigned char d[4] = { (unsigned char)psm, 0, (unsigned char)lcid[c], (unsigned char)(lcid[c] >> 8) };
    rcid[c] = cfg_in[c] = cfg_out[c] = 0; hs = c + 1; sig_id = sig_id % 255 + 1; sig(0x02, sig_id, d, 4);
}
static void let_go(void) {
    static const unsigned char up[8];
    hid_kbd(up, 8, kb_prev);
    for (unsigned k = 0; k < 3; k++) if (held >> k & 1) kinput(1, 272 + k, 0);
    if (held) kinput(0, 0, 0);
    held = 0;
}
static void configured(unsigned c) {
    if (!cfg_in[c] || !cfg_out[c]) return;
    if (c == 0) { hid_open(1); return; }
    static const unsigned char boot = 0x70;   /* SET_PROTOCOL: boot */
    l2_send(rcid[0], &boot, 1);
    hs = 3; until = 0;
    put("bt: "); put(peer_name[0] ? peer_name : kind(peer_cod)); put(" ready"); say(); term = 0;
}
static void on_sig(const unsigned char *p, unsigned n) {   /* signalling: commands back to back, each code, id, length, data */
    for (unsigned i = 0; i + 4 <= n;) {
        unsigned code = p[i], id = p[i + 1], len = p[i + 2] | p[i + 3] << 8u;
        const unsigned char *d = p + i + 4;
        if (i + 4 + len > n) return;
        i += 4 + len;
        int c = -1;
        if (code == 0x03 && len >= 8) {   /* Connection Response: dcid, scid, result, status */
            for (int k = 0; k < 2; k++) if ((unsigned)(d[2] | d[3] << 8u) == lcid[k]) c = k;
            if (c < 0) continue;
            unsigned res = d[4] | d[5] << 8u;
            if (res == 1) continue;   /* pending: the answer comes later */
            if (res) { fail(c ? "bt: the device refused its interrupt channel" : "bt: the device refused its control channel", (int)res); continue; }
            rcid[c] = d[0] | d[1] << 8u;
            unsigned char q[8] = { (unsigned char)rcid[c], (unsigned char)(rcid[c] >> 8), 0, 0, 0x01, 0x02, 0xA0, 0x02 };   /* flags 0, MTU 672 */
            sig_id = sig_id % 255 + 1; sig(0x04, sig_id, q, 8);
        } else if (code == 0x04 && len >= 4) {   /* Configuration Request for our CID: yes, success */
            for (int k = 0; k < 2; k++) if ((unsigned)(d[0] | d[1] << 8u) == lcid[k] && rcid[k]) c = k;
            if (c < 0) { unsigned char r[2] = { 0x02, 0 }; sig(0x01, id, r, 2); continue; }   /* invalid CID */
            unsigned char r[6] = { (unsigned char)rcid[c], (unsigned char)(rcid[c] >> 8), 0, 0, 0, 0 };
            sig(0x05, id, r, 6); cfg_in[c] = 1; configured((unsigned)c);
        } else if (code == 0x05 && len >= 6) {   /* Configuration Response: scid is ours */
            for (int k = 0; k < 2; k++) if ((unsigned)(d[0] | d[1] << 8u) == lcid[k]) c = k;
            if (c < 0) continue;
            if (d[4] | d[5]) { fail("bt: the device would not configure its HID channel", d[4] | d[5] << 8); continue; }
            cfg_out[c] = 1; configured((unsigned)c);
        } else if (code == 0x0A && len >= 2) {   /* Information Request: not supported */
            unsigned char r[4] = { d[0], d[1], 0x01, 0x00 }; sig(0x0B, id, r, 4);
        } else if (code == 0x06 && len >= 4) {   /* Disconnection Request: dcid, scid, echoed */
            unsigned char r[4] = { d[0], d[1], d[2], d[3] }; sig(0x07, id, r, 4);
        } else if (code == 0x01) { if (hs && hs < 3) fail("bt: the device rejected a HID request", d[0]); }
        else if (!(code & 1) && code != 0x02) { unsigned char r[2] = { 0, 0 }; sig(0x01, id, r, 2); }   /* a request we do not know */
        else if (code == 0x02) { unsigned char r[8] = { 0, 0, d[2], d[3], 0x02, 0, 0, 0 }; sig(0x03, id, r, 8); }   /* its own Connection Request: PSM not supported */
    }
}
static void on_data(unsigned cid, const unsigned char *p, unsigned n) {
    if (cid == 1) { on_sig(p, n); return; }
    if (hs != 3 || cid != lcid[1] || n < 2 || p[0] != 0xA1) return;   /* DATA Input on interrupt; HANDSHAKE on control is not waited for */
    if (p[1] == 1 && n >= 10) hid_kbd(p + 2, 8, kb_prev);
    else if (p[1] == 2 && n >= 5) {
        if (p[3]) kinput(2, 0, (signed char)p[3]);
        if (p[4]) kinput(2, 1, (signed char)p[4]);
        if (n > 5 && p[5]) kinput(2, 8, (signed char)p[5]);
        for (unsigned k = 0; k < 3; k++) if ((p[2] ^ held) >> k & 1) kinput(1, 272 + k, p[2] >> k & 1);
        held = p[2] & 7u; kinput(0, 0, 0);
    }
}
static unsigned char l2[72]; static unsigned l2n, l2want;
int btc_acl(const unsigned char *p, unsigned n) {   /* bt.c: an ACL packet; 1 when it was on our link */
    if (n < 4 || st != UP || ((p[0] | p[1] << 8u) & 0xFFF) != handle) return 0;
    unsigned pb = p[1] >> 4 & 3, len = p[2] | p[3] << 8u;
    if (4 + len > n) return 1;
    if (pb != 1) { l2n = 0; l2want = len >= 4 ? 4 + (p[4] | p[5] << 8u) : 0; }
    if (!l2want || l2want > sizeof l2 || l2n + len > sizeof l2) { l2want = 0; return 1; }
    for (unsigned i = 0; i < len; i++) l2[l2n++] = p[4 + i];
    if (l2n < l2want) return 1;
    unsigned m = l2want; l2want = 0;
    on_data(l2[2] | l2[3] << 8u, l2 + 4, m - 4);
    wipe(l2, sizeof l2);
    return 1;
}

int btc_poll(void) {   /* bt.c's slot is free: a deadline, or the next command. 1 when one was sent */
    if (until && now() > until && !sent) {
        if (st == UP && hs && hs < 3) fail("bt: the device did not open its keyboard channels", -1);
        else if (st == FINDING) { list(); st = IDLE; term = 0; }
        else if (st == CONNECTING) { q_addr(0x0408, peer, 0, 0); fail("bt: no answer, is it on and in pairing mode?", -1); }   /* Create Connection Cancel */
        else if (st == PAIRING || st == ENCRYPTING) fail("bt: pairing took too long", -1);
        until = 0;
    }
    if (cqh == cqt) return 0;
    unsigned k = cqh++ % CQ;
    sent = cq_op[k]; bt_hci_cmd(cq_op[k], cq_p[k], cq_n[k]); wipe(cq_p[k], 24);
    return 1;
}
int btc_busy(void) { return st != IDLE || cqh != cqt || sent; }
void btc_status(void) {   /* for `bt`: the saved classic device and the link */
    unsigned char b[LINK_SIZE];
    if (link_load(b)) { put("bt: saved headphones or keyboard: "); put(b[30] ? (const char *)b + 30 : "unnamed"); put(st == UP ? ", connected" : ", not connected"); say_now(); }
    wipe(b, sizeof b);
}
