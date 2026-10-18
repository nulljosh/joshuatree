#ifndef ARM64_SUDO_H
#define ARM64_SUDO_H
/* Admins and sudo on the Pi (slice 3 of 6.0). JT/SUDOERS.TXT lists the admin names, one a line; JT/USERS.TXT keeps the
   record format the i386 kernel's auth.h writes. The first account ever made is an admin. */
#define SUDO_OK 0
#define SUDO_E_NOTADMIN 20   /* the password was right but the account is not an admin */
#define SUDO_E_ALREADY 21    /* already an admin */
#define SUDO_E_NOTLISTED 22  /* not an admin, so there is nothing to take away */
#define SUDO_E_LAST 23       /* the last admin cannot be removed */

void sudo_load(void);                       /* after accounts_load: reads JT/SUDOERS.TXT */
int sudo_is_admin(const char *name);
unsigned sudo_admin_count(void);
const char *sudo_admin_name(unsigned i);    /* "" when i is out of range */
int sudo_make(const char *name);            /* makeadmin */
int sudo_unmake(const char *name);          /* unadmin; refuses the last admin */
int sudo_first(const char *name);           /* the first account ever made becomes the only admin */
void sudo_scrub(const char *name);          /* a removed or newly made account must not inherit an old admin entry */
int sudo_check(const char *user, const char *pass);   /* the password, then the admin test; a right one remembers it */
void sudo_arm(const char *user);            /* remember user's sudo now (the first account just made it) */
int sudo_active(void);                      /* the signed-in user proved a password less than SUDO_TTL ago */
void sudo_drop(void);                       /* sudo -k, and every sign-out */
int sudo_may_write(void);                   /* settings: no accounts, or the signed-in user is an admin */
const char *sudo_reason(int code);

unsigned long sudo_now_ms(void);            /* main.c (login_ui.h): the generic counter in milliseconds; a host test stubs it */
unsigned sudo_ttl_ms(void);                 /* main.c: 300000, or the check build's shorter time */
#endif
