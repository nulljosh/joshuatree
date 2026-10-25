#ifndef AUTH_H
#define AUTH_H
/* kernel/auth.c: desktop accounts (USERS.TXT, PBKDF2), the login screen and the gate gui_run passes through. */
#define AUTH_USERNAME_MAX 24
#define AUTH_PASSWORD_MAX 64
extern int auth_logged_in;                              /* 1 once a login or first run succeeded this boot */
extern char auth_current_user[AUTH_USERNAME_MAX + 1];   /* who; "" before that */
int auth_create_user(const char *username, const char *password);
int auth_change_password(const char *username, const char *old_password, const char *new_password);
int auth_field_input(char *out, int max, int y, int masked);
void auth_login_screen(void);
int auth_gate_would_prompt(void);
void auth_gate(void);
#endif
