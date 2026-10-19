#ifndef ARM64_HOME_H
#define ARM64_HOME_H
/* Home folders on the Pi (slice 4 of 6.0). With accounts, a signed-in person's files live in JT/HOME/<dir>/, where <dir> is
   the account name when it fits 8.3 (up to 8 characters) and the first five characters, a tilde and two hex digits of a hash
   when it does not. Apps (ufile.c), `~` in the Terminal and Samantha's [[save]] go there. Another person's folder opens only
   under sudo, so only for an admin who typed a password in the last five minutes. With no accounts nothing changes. */
#define HOME_DIR_MAX 8

unsigned home_dirname(const char *user, char out[HOME_DIR_MAX + 1]);   /* the folder name for an account name */
int home_active(void);                       /* accounts exist and somebody is signed in */
int home_mine(char out[HOME_DIR_MAX + 1]);   /* the signed-in person's folder name; 0 when there is none */
int home_collides(const char *name);         /* another account already uses name's folder name */
int home_make(const char *user);             /* JT/HOME/<dir> made when missing; 1 when it is there afterwards */
int home_in(const char *dir, int make);      /* standing in JT/: step into HOME/<dir>; 1, or 0 with the cursor unmoved from where it was */
int home_may(const char *dir);               /* no accounts, the signed-in person's own folder, or sudo is active */
int home_expand(const char *in, char *out, unsigned cap);   /* `~` and `~/x` become JT/HOME/<dir>[/x]; other paths are copied. 0: no home, -2: too long */
int home_walk(char *path, int keep, char **leaf);   /* from the card's root down to the folder holding path's last `keep` parts: 1, 0 not there, -1 another's home */
void home_back(void);                        /* after a 1 from home_walk: the cursor where it was */
int home_owns(const char *path);            /* 1 when path (from the root) is inside the signed-in person's own folder */
int home_op(int op, const char *path, const void *data, unsigned len);   /* 0 write, 1 rm, 2 mkdir at a path from the root: 1 ok, 0 failed, -1 another's home, -2 not an 8.3 name */
int home_save(const char *name, const void *data, unsigned len);       /* Samantha's [[save]]: ~/NAME; same answers as home_op */
#endif
