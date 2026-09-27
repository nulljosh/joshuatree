/* Built-in benchmarks. Run with "bench" on the multiboot command line
   (tools/bench.sh does this headless) or type bench in the shell.
   Numbers come from rdtsc, calibrated against the 100 Hz PIT so they
   read in microseconds no matter what the host clock is. Every line is
   printed to serial as "bench <name> <value> <unit>" and the run ends
   with "bench done", which is what the host script waits for. */

static inline unsigned long long bench_tsc(void){
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

static void bench_line(const char *name, unsigned int value, const char *unit){
    char tmp[12]; int i = 0; unsigned u = value;
    do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    char num[12]; int n = 0; while (i) num[n++] = tmp[--i]; num[n] = 0;
    serial_puts("bench "); serial_puts(name); serial_puts(" "); serial_puts(num);
    serial_puts(" "); serial_puts(unit); serial_puts("\n");
    puts(name); puts(": "); puts(num); puts(" "); puts(unit); puts("\n");
}

static void bench_yield_task(void){ for (;;) yield(); }

/* ponytail: 20 PIT ticks (200 ms) of calibration, good to a few percent, plenty for a trend line. */
/* No libgcc on this target, so no 64-bit division: every delta is
   truncated to 32 bits (fine below ~1.4 s at 3 GHz) and scaled per
   microsecond. */
static unsigned int bench_tsc_per_us(void){
    unsigned int t0 = ticks(); while (ticks() == t0) { }
    unsigned long long c0 = bench_tsc(); unsigned int t1 = ticks();
    while (ticks() < t1 + 20) { }
    unsigned int d = (unsigned int)(bench_tsc() - c0);
    return d / 200000u ? d / 200000u : 1;
}

static void bench_run(int have_disk){
    unsigned int boot_ms = ticks() * 10;
    unsigned int per_us = bench_tsc_per_us();
    unsigned long long c0, c1;
#define BENCH_D() ((unsigned int)(c1 - c0))

    /* heap: 20,000 alloc/free pairs, sizes cycling 32..1024 bytes */
    c0 = bench_tsc();
    for (unsigned int i = 0; i < 20000; i++) { void *p = kmalloc(32 + (i % 32) * 32); if (p) kfree(p); }
    c1 = bench_tsc();
    unsigned int heap_ns = (BENCH_D() / 20000u) * 1000u / per_us;

    /* memcpy: 256 KB moved 64 times, 16 MB total */
    unsigned int mb_s = 0;
    char *a = kmalloc(256 * 1024), *b = kmalloc(256 * 1024);
    if (a && b) {
        memset(a, 1, 256 * 1024);
        c0 = bench_tsc();
        for (int i = 0; i < 64; i++) memcpy(b, a, 256 * 1024);
        c1 = bench_tsc();
        unsigned int us = BENCH_D() / per_us;
        mb_s = us ? 16u * 1000000u / us : 0;
    }
    if (a) kfree(a); if (b) kfree(b);

    /* context switch: 10,000 round trips to a task that only yields */
    unsigned int switch_ns = 0;
    int id = task_create(bench_yield_task);
    if (id >= 0) {
        c0 = bench_tsc();
        for (int i = 0; i < 10000; i++) yield();
        c1 = bench_tsc();
        task_kill(id);
        switch_ns = (BENCH_D() / 20000u) * 1000u / per_us;
    }

    /* disk: 256 sectors, 128 KB, PIO */
    unsigned int disk_kb_s = 0;
    if (have_disk) {
        char sec[512];
        c0 = bench_tsc();
        for (unsigned int lba = 0; lba < 256; lba++) if (!ata_read_sector(lba, sec)) break;
        c1 = bench_tsc();
        unsigned int us = BENCH_D() / per_us;
        disk_kb_s = us ? 128u * 1000000u / us : 0;
    }

    bench_line("boot_to_shell", boot_ms, "ms");
    bench_line("heap_alloc_free", heap_ns, "ns/op");
    bench_line("memcpy", mb_s, "MB/s");
    bench_line("context_switch", switch_ns, "ns/switch");
    bench_line("disk_read", disk_kb_s, "KB/s");
    serial_puts("bench done\n");
#undef BENCH_D
}
