#!/usr/bin/env python3
"""RNG200 register harness: full seeds, bounded waits, health faults and wiped failures."""
import pathlib, subprocess, tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PI_BUILD
#define RNG_HOST_TEST
#include "arch/arm64/rng.c"
static unsigned ctrl, reads, mode, status_reads;
static unsigned long counter, frequency;
unsigned rng_read(unsigned off) {
    if (off == 0) return ctrl;
    if (off == 0x18) {
        status_reads++;
        if (mode == 2) return 0x20;
        if (mode == 3) return 0x80000000U;
        if (mode == 4 && reads == 2) return 0x20;
        return 0;
    }
    if (off == 0x24) return mode == 1 || (mode == 5 && reads == 1) ? 0x100 : 0x101;
    assert(off == 0x20); reads++; return 0x04030201U;
}
void rng_write(unsigned off, unsigned value) { assert(off == 0); ctrl = value; }
unsigned long rng_counter(void) { return counter++; }
unsigned long rng_frequency(void) { return frequency; }
static void reset(unsigned m) { mode=m; ctrl=0xA0001FFF; reads=status_reads=0; counter=0; frequency=100; }
int main(void) {
    unsigned char buf[34];
    for (unsigned n=1; n<=32; n++) {
        reset(0); memset(buf, 0xAA, sizeof buf);
        assert(tls_entropy(buf+1,n)); assert(ctrl == 0xA0000001);
        assert(reads == (n+3)/4 && buf[0] == 0xAA && buf[n+1] == 0xAA);
        for(unsigned i=0;i<n;i++) assert(buf[i+1] == i%4+1);
    }
    for(unsigned m=1;m<=5;m++) {
        reset(m); memset(buf,0xAA,sizeof buf);
        assert(!tls_entropy(buf+1,32)); assert(counter<=102);
        assert(buf[0]==0xAA && buf[33]==0xAA);
        for(unsigned i=1;i<=32;i++) assert(!buf[i]);
        if(m==2 || m==3) assert(!reads);
    }
    reset(0); counter=~0UL-2; assert(tls_entropy(buf,32)); /* counter wrap */
    reset(0); frequency=0; memset(buf,0xAA,32); assert(!tls_entropy(buf,32));
    for(unsigned i=0;i<32;i++) assert(!buf[i]);
    reset(0); assert(tls_entropy(buf,0)); assert(!reads && !status_reads);
    puts("PASS: RNG200 byte bounds, FIFO mask, health faults, partial failure wipe, timeout and counter wrap");
}
'''
with tempfile.TemporaryDirectory(prefix="jt-rng-") as tmp:
    c=pathlib.Path(tmp,"rng.c"); c.write_text(source)
    exe=str(pathlib.Path(tmp,"rng"))
    subprocess.run(["clang","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
                    "-I",str(root),str(c),"-o",exe],check=True)
    subprocess.run([exe],check=True,timeout=10)
