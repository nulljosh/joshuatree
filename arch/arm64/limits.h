#ifndef ARM64_LIMITS_H
#define ARM64_LIMITS_H
/* Parental controls on the Pi (12.0), as in Screen Time: an admin limits another account's apps, hours and web.
   JT/LIMITS.TXT, one line an account: "NAME apps=a,b,c hours=F-T web=0". limits.c keeps it; cmd.c's `limits` sets it. */
#define LIM_APPS_MAX 160

void limits_load(void);                          /* after sudo_load: reads JT/LIMITS.TXT */
int limits_app_ok(const char *app);             /* may the signed-in person open app now? 0 says why on the UART and screen */
int limits_web_ok(void);                        /* may the signed-in person load a page? */
int limits_set(const char *name, const char *field, const char *value);   /* field apps, hours, web or clear; 0 or ACC_E_* / LIM_E_* */
int limits_line(unsigned i, char *out, unsigned cap);   /* the i-th limited account as its file line; 0 past the end */
#define LIM_E_ADMIN 30    /* an admin is never limited */
#define LIM_E_VALUE 31    /* not a value that field takes */

int limits_hour(void);                           /* the local hour 0-23, or -1 while the clock is unknown; desk.c */
#endif
