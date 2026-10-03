#ifndef SHELLSYS_H
#define SHELLSYS_H
/* The allowlisted shell behind SYS_SHELL_RUN (shellsys.c). Runs `line` (a
   kernel copy it may modify), points *out at a static NUL-terminated buffer
   and returns its length. Never blocks, never sleeps, never touches the
   window system. */
unsigned int shellsys_run(char *line, char **out);
#endif
