#ifndef KLOG_H
#define KLOG_H
/* kernel/klog.c: the boot-time log `dmesg` prints and the clock's notification panel shows. */
void klog(const char *msg);            /* one line into the ring, tagged with the PIT tick */
void klog_dump(void);                  /* `dmesg`: every line, oldest first */
void gui_draw_notif_panel(void);       /* the panel under the menu-bar clock */
#endif
