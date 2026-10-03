#include "stdlib.h"
#include "ctype.h"
#include "string.h"
#include "../jtsys.h"

int atoi(const char *s) {
    return (int)strtol(s, 0, 10);
}

long strtol(const char *s, char **endptr, int base) {
    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    long v = 0;
    const char *start = s;
    for (;; s++) {
        int c = (unsigned char)*s, d;
        if (isdigit(c)) d = c - '0';
        else if (isalpha(c)) d = tolower(c) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (endptr) *endptr = (char *)(s == start ? start : s);
    return neg ? -v : v;
}

int abs(int v) { return v < 0 ? -v : v; }

void exit(int code) { jt_exit(code); }

/* First-fit heap over SYS_BRK. Every block carries a header {size, free};
   blocks are contiguous between heap_lo and heap_hi, so free() can merge
   with the next block in place and malloc() can split a large free one.
   brk_grow() asks the kernel for 64KB more at a time; if the first ask is
   refused the static arena stands in as the one and only region. */
struct jt_block { unsigned long size; unsigned long free; };
#define JT_ALIGN 8
#define JT_GROW (64u * 1024u)
JT_DATA static unsigned char jt_arena[JT_ARENA_SIZE];
JT_DATA static unsigned char *heap_lo = 0, *heap_hi = 0;
JT_DATA static int heap_is_arena = 0;
static unsigned long jt_align_up(unsigned long n) { return (n + (JT_ALIGN - 1)) & ~(unsigned long)(JT_ALIGN - 1); }

static int brk_grow(unsigned long need) {
    if (heap_is_arena) return 0;
    if (!heap_lo) {
        int top = jt_brk(0);
        if (top == 0 || (top < 0 && top > -4096)) { heap_lo = jt_arena; heap_hi = jt_arena + sizeof jt_arena; heap_is_arena = 1;
                        ((struct jt_block *)heap_lo)->size = sizeof jt_arena - sizeof(struct jt_block); ((struct jt_block *)heap_lo)->free = 1; return 1; }
        heap_lo = heap_hi = (unsigned char *)(unsigned)top;
    }
    unsigned long step = (need + sizeof(struct jt_block) + JT_GROW - 1) / JT_GROW * JT_GROW;
    int top = jt_brk((unsigned)heap_hi + (unsigned)step);
    if ((top < 0 && top > -4096) || top == 0 || (unsigned char *)(unsigned)top != heap_hi + step) {
        if (heap_lo == heap_hi) { heap_lo = jt_arena; heap_hi = jt_arena + sizeof jt_arena; heap_is_arena = 1;
                                  ((struct jt_block *)heap_lo)->size = sizeof jt_arena - sizeof(struct jt_block); ((struct jt_block *)heap_lo)->free = 1; return 1; }
        return 0;
    }
    struct jt_block *b = (struct jt_block *)heap_hi;
    b->size = step - sizeof *b; b->free = 1;
    heap_hi += step;
    /* merge with a free tail block */
    struct jt_block *p = (struct jt_block *)heap_lo, *prev = 0;
    while ((unsigned char *)p < (unsigned char *)b) { prev = p; p = (struct jt_block *)((unsigned char *)(p + 1) + p->size); }
    if (prev && prev->free) prev->size += sizeof *b + b->size;
    return 1;
}

void *malloc(unsigned long size) {
    if (size == 0) return 0;
    unsigned long need = jt_align_up(size);
    for (int pass = 0; pass < 2; pass++) {
        for (struct jt_block *b = (struct jt_block *)heap_lo; heap_lo && (unsigned char *)b < heap_hi;
             b = (struct jt_block *)((unsigned char *)(b + 1) + b->size)) {
            if (!b->free || b->size < need) continue;
            if (b->size >= need + sizeof *b + JT_ALIGN) {
                struct jt_block *rest = (struct jt_block *)((unsigned char *)(b + 1) + need);
                rest->size = b->size - need - sizeof *rest; rest->free = 1;
                b->size = need;
            }
            b->free = 0;
            return (void *)(b + 1);
        }
        if (!brk_grow(need)) return 0;
    }
    return 0;
}

void *calloc(unsigned long nmemb, unsigned long size) {
    unsigned long total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void free(void *ptr) {
    if (!ptr) return;
    struct jt_block *b = (struct jt_block *)ptr - 1;
    b->free = 1;
    struct jt_block *n = (struct jt_block *)((unsigned char *)(b + 1) + b->size);
    while ((unsigned char *)n < heap_hi && n->free) { b->size += sizeof *n + n->size; n = (struct jt_block *)((unsigned char *)(b + 1) + b->size); }
}

void *realloc(void *ptr, unsigned long size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return 0; }
    struct jt_block *b = (struct jt_block *)ptr - 1;
    if (b->size >= size) return ptr;
    void *n = malloc(size);
    if (!n) return 0;
    memcpy(n, ptr, b->size);
    free(ptr);
    return n;
}
