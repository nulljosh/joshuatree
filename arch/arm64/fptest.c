/* Test support for arm64-fp-check.py, linked into the fp build only (`make -C arch/arm64 fptest`), never the real kernel.
   fp_spin() fills q0-q31 with known patterns, sets FPCR and FPSR to known values, then waits in a loop for the timer
   interrupts; fp_clobber() is what main.c's interrupt handler calls in that build, standing in for a handler that uses
   floating point: it overwrites every one of those registers. If vectors.S keeps and restores them, fp_spin() finds
   everything as it left it and returns 0; otherwise it returns a nonzero mask of what changed. */
#define ALL_Q "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31"
#define SEED 0x0123456789abcdefUL
#define STEP 0x1111111111111111UL
#define FPCR_TEST 0x00400000UL   /* round toward plus infinity: not the reset value, so a lost FPCR shows */
#define FPSR_TEST 0x0800000fUL   /* QC and four sticky exception flags */

void fp_clobber(void) {
    __asm__ volatile (".irp n," ALL_Q "\n dup v\\n\\().2d, %0\n .endr\n msr fpcr, xzr\n msr fpsr, xzr" :: "r"(0xdeadbeefcafef00dUL)
        : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18",
          "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}

unsigned long fp_spin(volatile unsigned *t, unsigned want) {
    unsigned long bad;
    __asm__ volatile (
        "mov x9, %[seed]\n"
        ".irp n," ALL_Q "\n dup v\\n\\().2d, x9\n add x9, x9, %[step]\n .endr\n"
        "msr fpcr, %[cr]\n msr fpsr, %[sr]\n"
        "1: ldr w12, [%[t]]\n cmp w12, %w[want]\n b.lo 1b\n"                  /* spin until the timer has fired `want` times */
        "mov x9, %[seed]\n mov %[bad], xzr\n"
        ".irp n," ALL_Q "\n"
        " mov x13, v\\n\\().d[0]\n eor x13, x13, x9\n orr %[bad], %[bad], x13\n"
        " mov x13, v\\n\\().d[1]\n eor x13, x13, x9\n orr %[bad], %[bad], x13\n"
        " add x9, x9, %[step]\n .endr\n"
        "mrs x13, fpcr\n eor x13, x13, %[cr]\n orr %[bad], %[bad], x13\n"
        "mrs x13, fpsr\n eor x13, x13, %[sr]\n orr %[bad], %[bad], x13\n"
        : [bad] "=&r"(bad)
        : [seed] "r"(SEED), [step] "r"(STEP), [cr] "r"(FPCR_TEST), [sr] "r"(FPSR_TEST), [t] "r"(t), [want] "r"(want)
        : "x9", "x12", "x13", "cc", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14",
          "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
    return bad;
}
