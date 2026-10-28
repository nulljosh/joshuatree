#ifndef PK_STRING_H
#define PK_STRING_H
#include <stddef.h>
unsigned long strlen(const char *s); int strcmp(const char *a, const char *b); int strncmp(const char *a, const char *b, unsigned long n);
char *strchr(const char *s, int c); char *strrchr(const char *s, int c); char *strstr(const char *h, const char *n); char *strcpy(char *d, const char *s);
void *memcpy(void *d, const void *s, unsigned long n); void *memmove(void *d, const void *s, unsigned long n); void *memset(void *d, int c, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);
#endif
