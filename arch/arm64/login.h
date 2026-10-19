#ifndef ARM64_LOGIN_H
#define ARM64_LOGIN_H
/* The login screen's logic (slice 2 of 6.0): two fields, a name and a masked password, and what Enter does. Drawing is
   login_ui.h; the keys come from main.c's input_event. The password lives only in a static buffer here and is wiped the
   moment it has been checked. */
#define LOGIN_NONE 0     /* nothing changed */
#define LOGIN_REDRAW 1   /* the fields or the message changed */
#define LOGIN_DONE 2     /* a right password: current_user is set */
#define LOGIN_DELAY_MS 1000   /* every wrong password costs this much, so guessing is slow */

extern char current_user[];   /* the signed-in name, "" when nobody is */
void login_reset(void);       /* an empty form, nobody signed in */
int login_key(int ch);        /* a typed character, '\b', '\t' or '\n' */
const char *login_name(void);
unsigned login_pass_len(void);
int login_field(void);        /* 0 name, 1 password */
int login_message(void);      /* 1 after a wrong password, until the next key */
void login_wait_ms(unsigned ms);   /* main.c: a busy wait on the generic counter (a host test stubs it) */
#endif
