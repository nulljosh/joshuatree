/* 10.3.0, the battery. A Pi laptop or UPS board reports its cell through a MAX17048 fuel gauge at I2C address 0x36 on
   the HAT pins (GPIO 2 and 3, BSC1), as Geekworm's boards do: VCELL (register 0x02, 78.125 uV a step) and SOC (0x04, the
   percent in the high byte). Read once at boot and once a minute; with no gauge the menu bar keeps its power symbol.
   ponytail: one chip, one bus. Another gauge (an INA219, a PiSugar) is another reader beside bat_read. */
static int bat_pct = -1, bat_mv;   /* -1: no battery found */
/* register bytes 0x02 to 0x05 in, the percent out (0 to 100), or -1 when VCELL reads zero (no cell) */
static int bat_parse(const unsigned char r[4], int *mv) {
    unsigned v = (unsigned)r[0] << 8 | r[1];
    if (!v) return -1;
    *mv = (int)(v * 5 / 64);   /* 78.125 uV = 5/64 mV */
    int p = r[2] + (r[3] >= 128);   /* the low byte is 1/256 of a percent: rounded */
    return p > 100 ? 100 : p;   /* the gauge reads a little over 100 on a full charge */
}
#ifdef PI_BUILD
#define BSC1 0xFE804000UL
unsigned long cnt_now(void); unsigned long cnt_hz(void);
static int i2c_xfer(unsigned addr, int rd, unsigned char *b, unsigned n) {   /* 1 when all n bytes went, 0 on a NACK or 10 ms gone */
    volatile unsigned *r = (volatile unsigned *)BSC1;
    r[1] = 1u << 9 | 1u << 8 | 1u << 1;   /* S: clear CLKT, ERR and DONE */
    r[0] = 1u << 4;                        /* C: clear the FIFO */
    r[3] = addr; r[2] = n;
    r[0] = 1u << 15 | 1u << 7 | (rd ? 1u : 0);   /* I2CEN, ST, READ */
    if (!rd) for (unsigned i = 0; i < n; i++) r[4] = b[i];   /* after ST: QEMU's model sends a FIFO byte only once the transfer runs; the chip takes it either way */
    unsigned long end = cnt_now() + cnt_hz() / 100; unsigned got = 0;
    for (;;) {
        while (rd && got < n && (r[1] & 1u << 5)) b[got++] = (unsigned char)r[4];   /* RXD */
        if (r[1] & (1u << 1 | 1u << 8 | 1u << 9)) break;                         /* DONE, ERR or CLKT */
        if ((long)(cnt_now() - end) > 0) break;
    }
    while (rd && got < n && (r[1] & 1u << 5)) b[got++] = (unsigned char)r[4];
    int ok = !(r[1] & (1u << 8 | 1u << 9)) && (r[1] & 1u << 1) && (!rd || got == n);
    r[1] = 1u << 9 | 1u << 8 | 1u << 1;
    return ok;
}
static int bat_read(int *mv) {
    static int pins;
    if (!pins) {   /* GPIO 2 and 3 to alt 0, SDA1 and SCL1: GPFSEL0 bits 6 to 11 */
        volatile unsigned *g = (volatile unsigned *)0xFE200000UL;
        g[0] = (g[0] & ~0xFC0u) | 4u << 6 | 4u << 9; pins = 1;
    }
    unsigned char reg = 0x02, r[4];
    if (!i2c_xfer(0x36, 0, &reg, 1) || !i2c_xfer(0x36, 1, r, 4)) return -1;
    return bat_parse(r, mv);
}
#endif
