#ifndef SHELL_H
#define SHELL_H
/* kernel/shell.c: the shell's dispatcher, and what it calls back into kernel.c for. */
void shell_line(char *line);   /* run one typed line (the buffer is split in place) */
void show_time(void);
void beep(unsigned int freq_hz, unsigned int duration_ticks);
void reboot(void);
void gui_run(void);
extern int fs_ok_global;       /* the disk mounted at boot */
#endif
