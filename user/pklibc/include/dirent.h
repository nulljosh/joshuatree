#ifndef PK_DIRENT_H
#define PK_DIRENT_H
typedef struct { int unused; } DIR;
struct dirent { char d_name[64]; };
DIR *opendir(const char *p); struct dirent *readdir(DIR *d); int closedir(DIR *d);
#endif
