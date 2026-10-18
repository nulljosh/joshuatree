#ifndef ARM64_ACCOUNTS_H
#define ARM64_ACCOUNTS_H
/* User accounts on the Pi (slice 1 of 6.0): names and salted, iterated password hashes in JT/USERS.TXT. */
#define ACC_OK 0
#define ACC_E_NAME 1       /* the name is not 1 to 24 characters of a-z 0-9 _ - */
#define ACC_E_PASS 2       /* the password is not 4 to 64 characters */
#define ACC_E_DUP 3        /* that name is taken */
#define ACC_E_FULL 4       /* eight accounts already */
#define ACC_E_NOCARD 5     /* no SD card, so nothing could be kept */
#define ACC_E_WRITE 6      /* the card refused the write; nothing changed */
#define ACC_E_NOUSER 7     /* no account with that name */
#define ACC_E_WRONG 8      /* the password does not match */
#define ACC_E_ENTROPY 9    /* no random salt could be made */
#define ACC_USERS_MAX 8
#define ACC_NAME_MAX 24
#define ACC_PASS_MIN 4
#define ACC_PASS_MAX 64

void accounts_load(void);                 /* after sd_mount: reads JT/USERS.TXT; a missing or bad file means no accounts */
unsigned accounts_count(void);
const char *accounts_name(unsigned i);    /* "" when i is out of range */
int accounts_add(const char *name, const char *pass);
int accounts_check(const char *name, const char *pass);
int accounts_remove(const char *name, const char *pass);
const char *accounts_reason(int code);    /* a short plain sentence for the Terminal */
void accounts_wipe(void *p, unsigned n);  /* zero a buffer so the compiler cannot drop it */
#endif
