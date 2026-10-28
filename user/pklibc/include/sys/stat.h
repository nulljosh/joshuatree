#ifndef PK_SYS_STAT_H
#define PK_SYS_STAT_H
struct stat { unsigned st_mode; long long st_size; };
#define S_IFDIR 0040000
#define S_ISDIR(m) (((m) & 0170000) == S_IFDIR)
int stat(const char *p, struct stat *s); int mkdir(const char *p, unsigned m);
#endif
