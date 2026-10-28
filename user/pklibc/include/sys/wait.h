#ifndef PK_SYS_WAIT_H
#define PK_SYS_WAIT_H
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s) ((s) & 0x7f)
#endif
