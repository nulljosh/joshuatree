#ifndef PK_REGEX_H
#define PK_REGEX_H
/* No regular expressions on the box yet: regcomp says so, and Plank's runtime reports it as a bad pattern. */
#include <stddef.h>
typedef long regoff_t;
typedef struct { size_t re_nsub; } regex_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;
#define REG_EXTENDED 1
#define REG_NOTBOL 1
int regcomp(regex_t *r, const char *p, int f); int regexec(const regex_t *r, const char *s, size_t n, regmatch_t *m, int f);
void regfree(regex_t *r); size_t regerror(int e, const regex_t *r, char *b, size_t n);
#endif
