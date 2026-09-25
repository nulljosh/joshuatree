/* Host harness for libjt: compiles user/libjt/string.c and
   user/libjt/stdlib.c natively and checks their output on a table of
   cases whose expected values are the host libc's own documented
   behavior for the same call. It deliberately does NOT #include the
   host's <string.h>/<stdlib.h> alongside libjt's: both declare strlen()
   etc with the same names, and once both are linked into one binary the
   comparison stops proving anything about libjt specifically. So the
   "compare to host libc" here is a table of literal expected results,
   with the exact host libc semantics they encode named in each case.
   No QEMU, no i386 target: the fast inner loop, same pattern as
   tools/checks/png-host-check.sh. */
#include <stdio.h>

#include "libjt/string.h"
#include "libjt/stdlib.h"

static int failures = 0;

#define CHECK(desc, cond) do { \
    if (!(cond)) { printf("FAIL: %s\n", desc); failures++; } \
} while (0)

static void check_strlen(void) {
    CHECK("strlen empty", strlen("") == 0);
    CHECK("strlen hello", strlen("hello world") == 11);
}

static void check_strcmp(void) {
    CHECK("strcmp equal", strcmp("abc", "abc") == 0);
    CHECK("strcmp lt", strcmp("abc", "abd") < 0);
    CHECK("strcmp gt", strcmp("abd", "abc") > 0);
    CHECK("strncmp prefix", strncmp("abcxyz", "abcqqq", 3) == 0);
    CHECK("strncmp diff", strncmp("abcxyz", "abdqqq", 3) < 0);
}

static void check_strcpy(void) {
    char buf[16];
    strcpy(buf, "hi there");
    CHECK("strcpy", strcmp(buf, "hi there") == 0);
    char nbuf[8];
    for (unsigned i = 0; i < sizeof nbuf; i++) nbuf[i] = 'Z';
    strncpy(nbuf, "ab", sizeof nbuf);
    CHECK("strncpy copies", nbuf[0] == 'a' && nbuf[1] == 'b');
    CHECK("strncpy pads zero", nbuf[2] == 0 && nbuf[7] == 0);
}

static void check_strchr(void) {
    const char *s = "hello world";
    CHECK("strchr found", strchr(s, 'w') == s + 6);
    CHECK("strchr not found", strchr(s, 'z') == 0);
    CHECK("strchr NUL", *strchr(s, '\0') == '\0');
    CHECK("strrchr", strrchr(s, 'o') == s + 7);
    CHECK("strstr", strstr(s, "wor") == s + 6);
    CHECK("strstr empty needle", strstr(s, "") == s);
    CHECK("strstr not found", strstr(s, "xyz") == 0);
}

static void check_mem(void) {
    char a[8] = "abcdefg", b[8];
    memcpy(b, a, 8);
    CHECK("memcpy", memcmp(a, b, 8) == 0);
    memset(b, 'x', 4);
    CHECK("memset", b[0] == 'x' && b[3] == 'x' && b[4] == 'e');
    char c[11] = "0123456789";
    memmove(c + 2, c, 5);
    CHECK("memmove overlap forward", memcmp(c, "01012347", 8) == 0);
    char d[11] = "0123456789";
    memmove(d, d + 2, 5);
    CHECK("memmove overlap backward", memcmp(d, "2345656789", 10) == 0);
}

static void check_atoi(void) {
    CHECK("atoi positive", atoi("42") == 42);
    CHECK("atoi negative", atoi("-17") == -17);
    CHECK("atoi leading space", atoi("   99") == 99);
    CHECK("atoi garbage tail", atoi("42abc") == 42);
    CHECK("strtol base16", strtol("ff", 0, 16) == 255);
    CHECK("strtol base0 hex", strtol("0x1A", 0, 0) == 26);
    CHECK("strtol base0 octal", strtol("017", 0, 0) == 15);
    CHECK("strtol base10 neg", strtol("-123", 0, 10) == -123);
    CHECK("abs pos", abs(5) == 5);
    CHECK("abs neg", abs(-5) == 5);
}

static void check_malloc(void) {
    void *p = malloc(32);
    CHECK("malloc nonnull", p != 0);
    void *q = calloc(4, 8);
    CHECK("calloc nonnull", q != 0);
    unsigned char *cq = (unsigned char *)q;
    int zero_ok = 1;
    for (int i = 0; i < 32; i++) if (cq[i] != 0) zero_ok = 0;
    CHECK("calloc zeroed", zero_ok);
    free(p);
    void *p2 = malloc(32);
    CHECK("malloc reuses freed block", p2 == p);
    void *r = realloc(q, 64);
    CHECK("realloc nonnull", r != 0);
    unsigned char *cr = (unsigned char *)r;
    int preserved = 1;
    for (int i = 0; i < 32; i++) if (cr[i] != 0) preserved = 0;
    CHECK("realloc preserves contents", preserved);
    free(r);
    free(p2);
}

int main(void) {
    check_strlen();
    check_strcmp();
    check_strcpy();
    check_strchr();
    check_mem();
    check_atoi();
    check_malloc();

    if (failures) {
        printf("libjt-host-check: %d failure(s)\n", failures);
        return 1;
    }
    printf("libjt-host-check: all checks passed\n");
    return 0;
}
