/* The login form's state machine. Name: 1 to 24 of a-z 0-9 _ - (capitals are folded). Password: up to 64 characters, any
   printable. Enter in the name field moves to the password; Enter in the password checks it with accounts_check, wipes it
   and either signs in or waits LOGIN_DELAY_MS, clears the password and sets the message. The message never says whether the
   name or the password was wrong, and nothing here prints or logs a typed character.
   tools/checks/arm64-login-check.py runs this file on the host, then the whole screen in QEMU. */
#include "login.h"
#include "accounts.h"

char current_user[ACC_NAME_MAX + 1];
static char name[ACC_NAME_MAX + 1], pass[ACC_PASS_MAX + 1];
static unsigned nl, pl;
static int field, msg;

void login_reset(void) {
    accounts_wipe(pass, sizeof pass); accounts_wipe(name, sizeof name);
    accounts_wipe(current_user, ACC_NAME_MAX + 1);
    nl = pl = 0; field = 0; msg = 0;
}
const char *login_name(void) { return name; }
unsigned login_pass_len(void) { return pl; }
int login_field(void) { return field; }
int login_message(void) { return msg; }

int login_key(int ch) {
    int had = msg; msg = 0;
    if (ch == '\t') { field = !field; return LOGIN_REDRAW; }
    if (ch == '\b') {
        if (field == 0 && nl) name[--nl] = 0;
        else if (field == 1 && pl) pass[--pl] = 0;
        else return had ? LOGIN_REDRAW : LOGIN_NONE;
        return LOGIN_REDRAW;
    }
    if (ch == '\n') {
        if (field == 0) { if (nl) field = 1; return nl || had ? LOGIN_REDRAW : LOGIN_NONE; }
        if (!pl) return had ? LOGIN_REDRAW : LOGIN_NONE;
        pass[pl] = 0;
        int r = accounts_check(name, pass);
        accounts_wipe(pass, sizeof pass); pl = 0;           /* right or wrong, the typed password is gone */
        if (r == ACC_OK) {
            for (unsigned i = 0; i <= nl; i++) current_user[i] = name[i];
            accounts_wipe(name, sizeof name); nl = 0; field = 0;
            return LOGIN_DONE;
        }
        login_wait_ms(LOGIN_DELAY_MS);
        msg = 1;
        return LOGIN_REDRAW;
    }
    if (ch < 32 || ch > 126) return had ? LOGIN_REDRAW : LOGIN_NONE;
    if (field == 0) {
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok || nl >= ACC_NAME_MAX) return had ? LOGIN_REDRAW : LOGIN_NONE;
        name[nl++] = (char)ch; name[nl] = 0;
    } else {
        if (pl >= ACC_PASS_MAX) return had ? LOGIN_REDRAW : LOGIN_NONE;
        pass[pl++] = (char)ch; pass[pl] = 0;
    }
    return LOGIN_REDRAW;
}
