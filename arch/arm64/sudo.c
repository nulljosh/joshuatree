/* Admins and sudo. JT/SUDOERS.TXT holds the admin names, one a line. The file is untrusted: a line that is not an existing
   account's name is dropped, a repeat is kept once. A card with accounts but no SUDOERS.TXT (made before 4.10.0) makes the
   first account in USERS.TXT the admin, in memory, so nobody is locked out. A change goes to the card first and is kept
   in memory only if the write worked.
   sudo_check is the one place a sudo password is judged: accounts_check on the signed-in name, a one second wait when it is
   wrong (as the login screen does), then the admin test. The success is remembered for sudo_ttl_ms (five minutes) for that
   one name; sudo -k and every sign-out forget it. The password never lives here: the caller owns and wipes it.
   tools/checks/arm64-sudo-check.py runs this file on the host under sanitizers. */
#include "sudo.h"
#include "accounts.h"
#include "login.h"
#include "emmc.h"
#include "../../drivers/fat.h"

#define SUDO_FILE "SUDOERS.TXT"
static char adm[ACC_USERS_MAX][ACC_NAME_MAX + 1];
static unsigned nadm;
static char el_user[ACC_NAME_MAX + 1];
static unsigned long el_at;
static int el_on;

static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void copy(char *d, const char *s) { unsigned i = 0; for (; s[i] && i < ACC_NAME_MAX; i++) d[i] = s[i]; d[i] = 0; }
static int at(const char *name) { for (unsigned i = 0; i < nadm; i++) if (same(adm[i], name)) return (int)i; return -1; }

static int store(char list[][ACC_NAME_MAX + 1], unsigned n) {
    static char buf[ACC_USERS_MAX * (ACC_NAME_MAX + 1) + 1];
    if (!sd_ready()) return ACC_E_NOCARD;
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) { for (unsigned c = 0; list[i][c]; c++) buf[k++] = list[i][c]; buf[k++] = '\n'; }
    if (!k) buf[k++] = '\n';
    return sd_write(SUDO_FILE, buf, k) == 1 ? SUDO_OK : ACC_E_WRITE;
}
void sudo_load(void) {
    static char buf[1024];
    nadm = 0; el_on = 0;
    int got = 0;
    if (sd_ready() && sd_enter_jt(0)) { got = fat_read_file(SUDO_FILE, buf, sizeof buf); fat_chdir(".."); }
    for (int i = 0; i < got && nadm < ACC_USERS_MAX;) {
        int s = i;
        while (i < got && buf[i] != '\n') i++;
        int e = i; if (i < got) i++;
        if (e > s && buf[e - 1] == '\r') e--;
        char nm[ACC_NAME_MAX + 2]; int n = e - s;
        if (n <= 0 || n > ACC_NAME_MAX) continue;
        for (int k = 0; k < n; k++) nm[k] = buf[s + k];
        nm[n] = 0;
        if (accounts_has(nm) && at(nm) < 0) copy(adm[nadm++], nm);
    }
    if (!nadm && accounts_count()) copy(adm[nadm++], accounts_name(0));
}
int sudo_is_admin(const char *name) { return name[0] && at(name) >= 0; }
unsigned sudo_admin_count(void) { return nadm; }
const char *sudo_admin_name(unsigned i) { return i < nadm ? adm[i] : ""; }

int sudo_make(const char *name) {
    if (!accounts_has(name)) return ACC_E_NOUSER;
    if (at(name) >= 0) return SUDO_E_ALREADY;
    if (nadm >= ACC_USERS_MAX) return ACC_E_FULL;
    char all[ACC_USERS_MAX][ACC_NAME_MAX + 1];
    for (unsigned i = 0; i < nadm; i++) copy(all[i], adm[i]);
    copy(all[nadm], name);
    int r = store(all, nadm + 1);
    if (r) return r;
    copy(adm[nadm++], name);
    return SUDO_OK;
}
int sudo_unmake(const char *name) {
    int a = at(name);
    if (a < 0) return accounts_has(name) ? SUDO_E_NOTLISTED : ACC_E_NOUSER;
    if (nadm == 1) return SUDO_E_LAST;
    char all[ACC_USERS_MAX][ACC_NAME_MAX + 1];
    unsigned m = 0;
    for (unsigned i = 0; i < nadm; i++) if ((int)i != a) copy(all[m++], adm[i]);
    int r = store(all, m);
    if (r) return r;
    for (unsigned i = 0; i < m; i++) copy(adm[i], all[i]);
    nadm = m;
    if (el_on && same(el_user, name)) el_on = 0;
    return SUDO_OK;
}
int sudo_first(const char *name) {
    char all[1][ACC_NAME_MAX + 1];
    copy(all[0], name);
    int r = store(all, 1);
    copy(adm[0], name); nadm = 1;      /* the account exists already: it is an admin even if the card refused this line */
    return r;
}
void sudo_scrub(const char *name) {
    int a = at(name);
    if (a < 0) return;
    char all[ACC_USERS_MAX][ACC_NAME_MAX + 1];
    unsigned m = 0;
    for (unsigned i = 0; i < nadm; i++) if ((int)i != a) copy(all[m++], adm[i]);
    store(all, m);                     /* best effort: sudo_load drops names that are no account */
    for (unsigned i = 0; i < m; i++) copy(adm[i], all[i]);
    nadm = m;
    if (el_on && same(el_user, name)) el_on = 0;
}
void sudo_arm(const char *user) { copy(el_user, user); el_at = sudo_now_ms(); el_on = 1; }
int sudo_check(const char *user, const char *pass) {
    int r = accounts_check(user, pass);
    if (r) { login_wait_ms(LOGIN_DELAY_MS); return r; }
    if (!sudo_is_admin(user)) return SUDO_E_NOTADMIN;
    sudo_arm(user);
    return SUDO_OK;
}
int sudo_active(void) {
    if (!el_on) return 0;
    if (!current_user[0] || !same(el_user, current_user) || !sudo_is_admin(el_user) || sudo_now_ms() - el_at >= sudo_ttl_ms()) { el_on = 0; return 0; }
    return 1;
}
void sudo_drop(void) { el_on = 0; accounts_wipe(el_user, sizeof el_user); }
int sudo_may_write(void) { return !accounts_count() || sudo_is_admin(current_user); }
const char *sudo_reason(int code) {
    switch (code) {
    case SUDO_E_NOTADMIN: return "not an admin";
    case SUDO_E_ALREADY: return "already an admin";
    case SUDO_E_NOTLISTED: return "not an admin, nothing to take away";
    case SUDO_E_LAST: return "that is the last admin, make another one first";
    }
    return accounts_reason(code);
}
