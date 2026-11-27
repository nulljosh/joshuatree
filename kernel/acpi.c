/* 13.25.0: Shut Down really turns the PC off (issue #1018). Before this it halted the CPU on "It's now safe to
   turn off this computer" and the power stayed on. ACPI's soft-off, read from the firmware's own tables, not
   hardcoded per chipset:
     1. find the RSDP ("RSD PTR ", checksummed) in the EBDA's first KiB or the BIOS area 0xE0000-0xFFFFF;
     2. walk its RSDT to the FADT ("FACP"), which gives PM1a_CNT (and PM1b_CNT), SMI_CMD and ACPI_ENABLE;
     3. find \_S5_ in the DSDT's AML: a Name holding a Package whose first two numbers are SLP_TYPa and SLP_TYPb;
     4. switch the chipset into ACPI mode if SCI_EN is clear, then write SLP_TYPx << 10 | SLP_EN to PM1x_CNT.
   Tables above the base 8 MB map are reached through paging_map_region (identity), as the framebuffer is.
   Returns only when the machine is still on, so every caller keeps its old halt as the fallback.
   tools/checks/acpi-off-check.py boots `poweroff` on QEMU's pc and q35 machines and waits for QEMU to exit. */
#include "acpi.h"
#include "paging.h"
#include "serial.h"

static inline void outb(unsigned short p, unsigned char v) { __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outw(unsigned short p, unsigned short v) { __asm__ volatile ("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline unsigned short inw(unsigned short p) { unsigned short v; __asm__ volatile ("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static int sig(const unsigned char *p, const char *s) { for (int i = 0; s[i]; i++) if (p[i] != (unsigned char)s[i]) return 0; return 1; }
static int sum_ok(const unsigned char *p, unsigned n) { unsigned char s = 0; while (n--) s += *p++; return s == 0; }
static void hex(const char *k, unsigned v) {
    char b[11] = "0x"; for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15]; b[10] = 0;
    serial_puts(k); serial_puts(b);
}

static const unsigned char *rsdp_in(unsigned from, unsigned len) {
    for (unsigned a = from; a + 20 <= from + len; a += 16) {
        const unsigned char *p = (const unsigned char *)a;
        if (sig(p, "RSD PTR ") && sum_ok(p, 20)) return p;
    }
    return 0;
}
/* A table's header, mapped and checked: its signature, a sane length and its checksum. */
static const unsigned char *table(unsigned phys, const char *want) {
    if (phys < 0x1000 || !paging_map_region(phys, 36)) return 0;
    const unsigned char *t = (const unsigned char *)phys;
    unsigned len = rd32(t + 4);
    if (want && !sig(t, want)) return 0;
    if (len < 36 || len > (1u << 20) || !paging_map_region(phys, len) || !sum_ok(t, len)) return 0;
    return t;
}

int acpi_poweroff(void) {
    unsigned ebda = (unsigned)*(const unsigned short *)0x40E << 4;
    const unsigned char *rsdp = (ebda >= 0x80000 && ebda < 0xA0000) ? rsdp_in(ebda, 1024) : 0;
    if (!rsdp) rsdp = rsdp_in(0xE0000, 0x20000);
    if (!rsdp) { serial_puts("acpi: no RSDP\n"); return -1; }
    const unsigned char *rsdt = table(rd32(rsdp + 16), "RSDT"), *fadt = 0;
    for (unsigned i = 36; rsdt && i + 4 <= rd32(rsdt + 4) && !fadt; i += 4) {
        unsigned a = rd32(rsdt + i);
        if (a >= 0x1000 && paging_map_region(a, 36) && sig((const unsigned char *)a, "FACP")) fadt = table(a, "FACP");
    }
    if (!fadt || rd32(fadt + 4) < 72) { serial_puts("acpi: no FADT\n"); return -2; }
    unsigned smi = rd32(fadt + 48), pm1a = rd32(fadt + 64), pm1b = rd32(fadt + 68);
    unsigned char enable = fadt[52];
    const unsigned char *dsdt = table(rd32(fadt + 40), "DSDT");
    int typa = -1, typb = 0;
    for (unsigned i = 36 + 2; dsdt && i + 8 < rd32(dsdt + 4) && typa < 0; i++) {
        const unsigned char *s = dsdt + i;
        if (!sig(s, "_S5_") || s[4] != 0x12) continue;               /* PackageOp after the name */
        if (s[-1] != 0x08 && !(s[-2] == 0x08 && s[-1] == '\\')) continue;   /* NameOp, maybe rooted */
        s += 5;
        s += ((*s & 0xC0) >> 6) + 2;                                  /* PkgLength's bytes, then NumElements */
        if (*s == 0x0A) s++;                                          /* BytePrefix; ZeroOp and OneOp are 0 and 1 already */
        typa = *s++ & 7;
        if (*s == 0x0A) s++;
        typb = *s & 7;
    }
    if (typa < 0 || !pm1a) { serial_puts("acpi: no \\_S5_\n"); return -3; }
    if (!(inw((unsigned short)pm1a) & 1) && smi && enable) {         /* SCI_EN clear: legacy mode, ask the firmware */
        outb((unsigned short)smi, enable);
        for (int t = 0; t < 3000000 && !(inw((unsigned short)pm1a) & 1); t++) __asm__ volatile ("pause");
    }
    hex("acpi: power off pm1a=", pm1a); hex(" slp_typ=", (unsigned)typa); serial_puts("\n");
    __asm__ volatile ("cli");
    outw((unsigned short)pm1a, (unsigned short)(typa << 10 | 1 << 13));
    if (pm1b) outw((unsigned short)pm1b, (unsigned short)(typb << 10 | 1 << 13));
    for (int t = 0; t < 3000000; t++) __asm__ volatile ("pause");
    __asm__ volatile ("sti");
    serial_puts("acpi: still on\n");
    return -4;
}
