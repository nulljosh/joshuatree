#ifndef APPSFOLDER_H
#define APPSFOLDER_H
/* kernel/appsfolder.c: the Apps folder. */
extern int apps_vis_rows;      /* rows the open folder shows; gui_launch_from_dock sets it (apps_geom.h) */
void gui_launch_apps(void);    /* open it; returns when it closes */
#endif
