#ifndef SELFTEST_H
#define SELFTEST_H
int selftest_run(const char *line, char *arg);   /* kernel/selftest.c; 1: line was a self-test and ran */
int taskdemo_run(const char *line);              /* kernel/taskdemo.c; 1: line was a scheduler demo and ran */
int shellfs_run(const char *line, char *arg);    /* kernel/shellfs.c; 1: line was a file command and ran */
int shellnet_run(const char *line, char *arg);   /* kernel/shellnet.c; 1: line was a network command and ran */
int web_starts_with(const char *p, const char *needle);   /* kernel.c: p begins with needle */
void render_wrapped_text(const char *text, int x0, int y0, int max_w_px, int max_h_px, unsigned int fg);   /* kernel.c: word-wrapped page text in the open window */
int wxtest_run(const char *line, char *arg);      /* kernel/wxtest.c; 1: line was a weather or wallpaper self-test and ran */
int progtest_run(const char *line);   /* kernel/progtest.c; 1: line was usertest or notetest and ran */
void shell_line(char *line);         /* kernel.c: run(), the shell's dispatcher */
#endif
