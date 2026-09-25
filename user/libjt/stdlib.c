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

/* Bump allocator with a singly-linked free list. Each live or free block
   is preceded by a header carrying its usable size, so free()/realloc()
   need no separate bookkeeping table. */
struct jt_block { unsigned long size; struct jt_block *next_free; };

/* Flat user binaries get no .bss (see user/hello.ld): nothing zeroes it
   and there is no crt0 to do it at startup. Force this into .data, which
   the linker script does write out, so the arena is genuinely
   zero-filled bytes in the image rather than an uninitialised page. */
JT_DATA
static unsigned char jt_arena[JT_ARENA_SIZE];
static unsigned long jt_arena_used = 0;
JT_DATA
static struct jt_block *jt_free_list = 0;

#define JT_ALIGN 8
static unsigned long jt_align_up(unsigned long n) { return (n + (JT_ALIGN - 1)) & ~(unsigned long)(JT_ALIGN - 1); }

void *malloc(unsigned long size) {
    if (size == 0) return 0;
    unsigned long need = jt_align_up(size);

    /* First-fit reuse before growing the arena. */
    struct jt_block **prev = &jt_free_list;
    for (struct jt_block *b = jt_free_list; b; b = b->next_free) {
        if (b->size >= need) {
            *prev = b->next_free;
            return (void *)(b + 1);
        }
        prev = &b->next_free;
    }

    unsigned long total = sizeof(struct jt_block) + need;
    if (jt_arena_used + total > sizeof(jt_arena)) return 0; /* arena ceiling hit */
    struct jt_block *b = (struct jt_block *)(jt_arena + jt_arena_used);
    b->size = need;
    jt_arena_used += total;
    return (void *)(b + 1);
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
    b->next_free = jt_free_list;
    jt_free_list = b;
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
