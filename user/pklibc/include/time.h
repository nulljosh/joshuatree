#ifndef PK_TIME_H
#define PK_TIME_H
#include <stddef.h>
typedef long long time_t;
struct timespec { time_t tv_sec; long tv_nsec; };
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
#define CLOCK_REALTIME 0
time_t time(time_t *t); int clock_gettime(int c, struct timespec *t); int nanosleep(const struct timespec *t, struct timespec *r);
struct tm *localtime(const time_t *t); time_t mktime(struct tm *tm); char *strptime(const char *s, const char *f, struct tm *tm); size_t strftime(char *b, size_t n, const char *f, const struct tm *tm);
#endif
