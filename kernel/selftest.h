#ifndef SELFTEST_H
#define SELFTEST_H
int selftest_run(const char *line, char *arg);   /* kernel/selftest.c; 1: line was a self-test and ran */
int taskdemo_run(const char *line);              /* kernel/taskdemo.c; 1: line was a scheduler demo and ran */
int shellfs_run(const char *line, char *arg);    /* kernel/shellfs.c; 1: line was a file command and ran */
#endif
