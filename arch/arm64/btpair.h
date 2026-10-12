/* The saved Bluetooth pairing, JT/BTPAIR.BIN: what bt.c needs to reconnect a mouse after a reboot without scanning or
   pairing again. Pure functions on byte arrays, no kernel calls, so tools/checks/bt-pair-check.py builds this header on the
   host and round-trips it. The mouse is a Low Energy device, so the "link key" is the LTK with its EDIV and Rand.
   The record is 70 bytes, little endian: magic "JTBP", version, address type, address, LTK, Rand, EDIV, name (30 bytes, NUL
   padded), then a 16 bit sum of everything before it. Nothing here prints; the error words never carry a record's bytes. */
#ifndef BTPAIR_H
#define BTPAIR_H
#define BTP_SIZE 70
#define BTP_VERSION 1
#define BTP_NAME 30
struct btp { unsigned char type, addr[6], ltk[16], rand[8]; char name[BTP_NAME]; unsigned short ediv; };
enum { BTP_OK = 0, BTP_SHORT = -1, BTP_MAGIC = -2, BTP_VER = -3, BTP_SUM = -4, BTP_BAD = -5 };
static inline unsigned short btp_sum(const unsigned char *b, unsigned n) {
    unsigned s = 0x4A54;
    for (unsigned i = 0; i < n; i++) s = (s + b[i] * (i % 251 + 1)) & 0xFFFF;
    return (unsigned short)s;
}
static inline int btp_valid(const struct btp *r) {   /* a real address type, and a key that is not all zero */
    unsigned z = 0;
    for (unsigned i = 0; i < 16; i++) z |= r->ltk[i];
    return r->type <= 1 && z != 0;
}
static inline int btp_pack(unsigned char *out, const struct btp *r) {   /* BTP_SIZE bytes into out; BTP_BAD when r is not worth saving */
    if (!btp_valid(r)) return BTP_BAD;
    unsigned k = 0;
    out[k++] = 'J'; out[k++] = 'T'; out[k++] = 'B'; out[k++] = 'P'; out[k++] = BTP_VERSION; out[k++] = r->type;
    for (unsigned i = 0; i < 6; i++) out[k++] = r->addr[i];
    for (unsigned i = 0; i < 16; i++) out[k++] = r->ltk[i];
    for (unsigned i = 0; i < 8; i++) out[k++] = r->rand[i];
    out[k++] = (unsigned char)r->ediv; out[k++] = (unsigned char)(r->ediv >> 8);
    unsigned end = 0;   /* the name stops at its first NUL; the last byte is always NUL */
    for (unsigned i = 0; i < BTP_NAME - 1 && r->name[i]; i++) end = i + 1;
    for (unsigned i = 0; i < BTP_NAME; i++) out[k++] = i < end ? (unsigned char)r->name[i] : 0;
    unsigned short s = btp_sum(out, k);
    out[k++] = (unsigned char)s; out[k++] = (unsigned char)(s >> 8);
    return BTP_OK;
}
static inline int btp_parse(const unsigned char *b, unsigned n, struct btp *r) {   /* BTP_OK, or why not; r is untouched on an error */
    if (n < BTP_SIZE) return BTP_SHORT;
    if (b[0] != 'J' || b[1] != 'T' || b[2] != 'B' || b[3] != 'P') return BTP_MAGIC;
    if (b[4] != BTP_VERSION) return BTP_VER;
    if (btp_sum(b, BTP_SIZE - 2) != (unsigned short)(b[BTP_SIZE - 2] | b[BTP_SIZE - 1] << 8)) return BTP_SUM;
    struct btp t;
    unsigned k = 5;
    t.type = b[k++];
    for (unsigned i = 0; i < 6; i++) t.addr[i] = b[k++];
    for (unsigned i = 0; i < 16; i++) t.ltk[i] = b[k++];
    for (unsigned i = 0; i < 8; i++) t.rand[i] = b[k++];
    t.ediv = (unsigned short)(b[k] | b[k + 1] << 8); k += 2;
    for (unsigned i = 0; i < BTP_NAME; i++) t.name[i] = (char)b[k++];
    t.name[BTP_NAME - 1] = 0;
    if (!btp_valid(&t)) return BTP_BAD;
    *r = t;
    return BTP_OK;
}
static inline const char *btp_why(int e) {   /* one plain word for each error */
    return e == BTP_OK ? "ok" : e == BTP_SHORT ? "too short" : e == BTP_MAGIC ? "not a pairing file" : e == BTP_VER ? "a different version"
         : e == BTP_SUM ? "damaged" : "unusable";
}
#endif
