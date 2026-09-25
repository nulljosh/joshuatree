#ifndef LIBJT_UNISTD_H
#define LIBJT_UNISTD_H
/* libjt: thin Unix-shaped names over jtsys.h, for code that expects
   read()/write()/close() rather than the jt_ prefixed calls. */
#include "../jtsys.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

static inline int read(int fd, void *buf, unsigned long len)        { return jt_read(fd, buf, (unsigned)len); }
static inline int write(int fd, const void *buf, unsigned long len) { return jt_write(fd, buf, (unsigned)len); }
static inline int close(int fd)                                     { return jt_close(fd); }

#endif
