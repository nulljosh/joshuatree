#ifndef ARM64_SNAP_H
#define ARM64_SNAP_H
/* Home snapshots on the Pi (4.12.0). `snapshot` copies the signed-in person's home folder into ~/SNAP/<n>/, `snapshots` lists
   them and `restore <n> yes` puts the folder back as it was. The newest SNAP_KEEP are kept. They live inside the home so they
   are as private as the files they copy (home.c's rules), and are never copied into the next snapshot. A snapshot counts only
   once its _DONE file is written, last, so one cut short by a pulled card is ignored and cleaned up by the next. Files over
   SNAP_FILE_MAX bytes, and entries past SNAP_LIST_MAX in one folder, are skipped and counted, never half copied. */
#define SNAP_KEEP 3
#define SNAP_FILE_MAX 32768u
#define SNAP_LIST_MAX 24

/* 1 made (*id is its number), 0 the card refused, -1 no card, -2 nobody is signed in. *files and *skipped are counts. */
int snap_take(unsigned *id, unsigned *files, unsigned *skipped);
/* cb(id, info) for each finished snapshot, oldest first; returns how many, -1 no card, -2 nobody is signed in. */
int snap_list(void (*cb)(unsigned id, const char *info));
/* 1 restored, 0 the card refused, -1 no card, -2 nobody signed in, -3 no such snapshot. Small files the snapshot lacks are removed. */
int snap_restore(unsigned id, unsigned *files, unsigned *skipped);
/* backup (5.2.0): the same walk as a snapshot, packed for the relay to keep on the Mac. The container is "JTB1", then per file
   one byte of path length, the path (8.3 names joined by /), four bytes of size (little end first) and the bytes. Same limits
   as a snapshot, plus BACKUP_MAX for the whole: a file that would pass it is skipped and counted. 1 packed, 0 the card refused,
   -1 no card, -2 nobody is signed in. */
#define BACKUP_MAX (512u * 1024u)
int snap_pack(unsigned char *out, unsigned cap, unsigned *len, unsigned *files, unsigned *skipped);
#endif
