/* The Users pane of Settings (4.14.0), the sixth section, #included by settings_ui.h. It lists the accounts, marks the admins and
   adds, re-passwords, promotes and removes them, with the same code the Terminal's adduser, deluser, passwd, makeadmin and
   unadmin run (cmd.c: users_add, users_delete, users_passwd; sudo.c: sudo_make, sudo_unmake). Nothing here judges a password:
   a change that needs privilege asks the signed-in admin for theirs through sudo_check, the one place sudo is decided, and so
   shares sudo's remembered five minutes (sudo_active). A person who is not an admin sees the list and changes their own
   password with the old one, nothing else. With no accounts the pane says so and Add user makes the first, an admin.
   Views: the list, one account's page, a form (add user, change password), the admin password and the delete confirm. Up and
   Down pick a row, Return does it, Esc goes back one step (and from the list to the sidebar); a click does what Return does.
   A password is only ever stars on the screen, and while one is being typed no `key N` line reaches the UART (a key code is a
   letter); every buffer that held one is wiped when the view changes. The UART says what happened and to whom, never a
   password: `settings users view list`, `added NAME`, `passwd NAME`, `admin NAME on`, `deleted NAME`, `refused WHY`.
   tools/checks/arm64-users-check.py drives all of it in QEMU. */
int users_add(const char *name, const char *pass, void (*out)(const char *));   /* cmd.c: adduser's work, said through out */
int users_delete(const char *name, void (*out)(const char *));                  /* cmd.c: deluser's work for an admin, no password of the account's own */
int users_passwd(const char *name, const char *pass, void (*out)(const char *)); /* cmd.c: passwd's work once both new passwords agree */

enum { UV_LIST, UV_DETAIL, UV_FORM, UV_AUTH, UV_CONFIRM };
enum { UO_ADD, UO_PASS, UO_ADMIN, UO_DELETE };
enum { UD_PASS, UD_ADMIN, UD_DELETE, UD_BACK };
enum { UG_OK, UG_ASK, UG_NO };
static int us_focus, us_view, us_sel, us_op, us_hush;
static char us_who[ACC_NAME_MAX + 2];                 /* the account the detail page, form or confirm is about */
static char us_f[3][ACC_PASS_MAX + 2];                /* the form's fields: add (name, password, again), password (old, new, again) */
static unsigned us_fn[3];
static char us_auth[ACC_PASS_MAX + 2];                /* the admin's password, in the auth view only */
static unsigned us_an;
static char us_msg[72];
static char us_out[200];
static unsigned us_on;

static void us_cat(char *b, unsigned cap, const char *s) { unsigned n = 0; while (b[n]) n++; for (; *s && n + 1 < cap; s++) b[n++] = *s; b[n] = 0; }
#define US_CAT(b, s) us_cat(b, sizeof b, s)
static int us_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void us_copy(char *d, const char *s, unsigned cap) { unsigned i = 0; for (; s[i] && i + 1 < cap; i++) d[i] = s[i]; d[i] = 0; }
static void us_wipe(void) {
    accounts_wipe(us_f, sizeof us_f); accounts_wipe(us_auth, sizeof us_auth);
    us_fn[0] = us_fn[1] = us_fn[2] = 0; us_an = 0;
}
static void us_reset(void) { us_wipe(); us_focus = 0; us_view = UV_LIST; us_sel = 0; us_msg[0] = 0; us_hush = 0; us_who[0] = 0; }
static int us_me(const char *n) { return current_user[0] && us_eq(n, current_user); }
static int us_admin_viewer(void) { return current_user[0] && sudo_is_admin(current_user); }
static int us_secret_view(void) { return us_view == UV_FORM || us_view == UV_AUTH; }
int set_secret(void) { return settings_live && set_sec == SET_USERS && us_focus && (us_secret_view() || us_hush); }   /* main.c keeps `key N` lines off the UART while 1 */
static void us_log(const char *what, const char *name) { set_log(what, -1, name && name[0] ? name : 0); }
static void us_say(const char *m) { us_copy(us_msg, m, sizeof us_msg); }
static void us_leave_secret(void) { if (us_secret_view()) us_hush = 1; }   /* the key that closes a password view, and its release, stay off the UART too */

static int us_list_n(void) { unsigned c = accounts_count(); return (int)c + (c < ACC_USERS_MAX ? 1 : 0); }
static int us_first(void) { return us_op == UO_PASS && !us_me(us_who) ? 1 : 0; }   /* the form's first field: someone else's password has no "current" */
static int us_nfields(void) { return us_view == UV_AUTH ? 1 : 3 - us_first(); }
static int us_dids(int *ids) {   /* the account page's rows */
    int n = 0;
    ids[n++] = UD_PASS;
    if (us_admin_viewer()) { ids[n++] = UD_ADMIN; ids[n++] = UD_DELETE; }
    ids[n++] = UD_BACK;
    return n;
}
static int us_rows(void) {
    int ids[4];
    switch (us_view) {
    case UV_LIST: return us_list_n();
    case UV_DETAIL: return us_dids(ids);
    case UV_FORM: return us_nfields() + 2;
    case UV_AUTH: return 3;
    }
    return 2;   /* the delete confirm: Delete, Cancel */
}
static void us_view_set(int v, int sel, const char *log) {
    us_leave_secret();
    us_wipe(); us_view = v; us_sel = sel;
    us_log(log, v == UV_DETAIL || v == UV_CONFIRM ? us_who : 0);
    if (settings_live) set_paint();
}
static void us_to_list(int sel) { us_view_set(UV_LIST, sel, "users view list"); }

static int us_gate(void) {   /* who may change accounts: nobody is protected until there is an account; then an admin, who may owe a password */
    if (!accounts_count()) return UG_OK;
    if (!us_admin_viewer()) return UG_NO;
    return sudo_active() ? UG_OK : UG_ASK;
}
static void us_sink(const char *s) { for (; *s && us_on < sizeof us_out - 1; s++) us_out[us_on++] = *s; us_out[us_on] = 0; }
static void us_reason(void) {   /* the first line of what the command said that is not "working": the part after "verb NAME: ", capital first */
    const char *p = us_out;
    while (*p) {
        const char *e = p; int working = 0;
        while (*e && *e != '\n') { if (e[0] == 'w' && e[1] == 'o' && e[2] == 'r' && e[3] == 'k' && e[4] == 'i') working = 1; e++; }
        if (!working) {
            const char *c = p; while (c < e && !(c[0] == ':' && c[1] == ' ')) c++;
            if (c < e) c += 2; else c = p;
            unsigned k = 0; char t[sizeof us_msg];
            while (c < e && k + 1 < sizeof t) t[k++] = *c++;
            t[k] = 0;
            if (t[0] >= 'a' && t[0] <= 'z') t[0] = (char)(t[0] - 32);
            us_say(t); return;
        }
        p = *e ? e + 1 : e;
    }
    us_say("Nothing was changed.");
}
static void us_done(int ok, const char *good, int sel) {   /* back to the list with what happened */
    us_wipe();
    us_view_set(UV_LIST, sel, "users view list");
    if (ok) us_say(good); else us_reason();
    if (settings_live) set_paint();
}
static int us_pos(const char *name) { for (unsigned i = 0; i < accounts_count(); i++) if (us_eq(accounts_name(i), name)) return (int)i; return 0; }
static void us_run(void) {   /* the gate is open: do the pending change */
    char name[ACC_NAME_MAX + 2], good[sizeof us_msg];
    us_copy(name, us_who, sizeof name);
    if (us_op == UO_ADD) us_copy(name, us_f[0], sizeof name);
    us_out[0] = 0; us_on = 0; good[0] = 0;
    if (us_op == UO_ADD || us_op == UO_PASS) { us_say("Working, the password is being stretched..."); if (settings_live) set_paint(); }
    if (us_op == UO_ADD) {
        int r = users_add(name, us_f[1], us_sink);
        if (!r) { us_log("users added", name); us_copy(good, "Added ", sizeof good); US_CAT(good, name); US_CAT(good, "."); }
        else us_log("users refused add", 0);
        us_done(!r, good, r ? 0 : us_pos(name));
    } else if (us_op == UO_PASS) {
        int r = users_passwd(name, us_f[1], us_sink);
        if (!r) { us_log("users passwd", name); us_copy(good, "Password changed for ", sizeof good); US_CAT(good, name); US_CAT(good, "."); }
        else us_log("users refused passwd", 0);
        us_done(!r, good, us_pos(name));
    } else if (us_op == UO_ADMIN) {
        int was = sudo_is_admin(name), r = was ? sudo_unmake(name) : sudo_make(name);
        us_leave_secret(); us_wipe(); us_view = UV_DETAIL; us_sel = 1; us_log("users view detail", name);
        if (!r) { us_log(was ? "users admin off" : "users admin on", name); US_CAT(good, name); US_CAT(good, was ? " is no longer an admin." : " is now an admin."); us_say(good); }
        else { us_log("users refused admin", 0); us_say(sudo_reason(r)); if (us_msg[0] >= 'a' && us_msg[0] <= 'z') us_msg[0] = (char)(us_msg[0] - 32); }
        if (settings_live) set_paint();
    } else {
        int r = users_delete(name, us_sink);
        if (!r) { us_log("users deleted", name); us_copy(good, "Removed ", sizeof good); US_CAT(good, name); US_CAT(good, "."); }
        else us_log("users refused delete", 0);
        us_done(!r, good, 0);
    }
}
static void us_auth_submit(void) {   /* the admin's password, judged by sudo_check as the Terminal's sudo does; a right one is remembered for sudo's five minutes */
    us_say("Checking the password..."); if (settings_live) set_paint();
    int r = sudo_check(current_user, us_auth);
    accounts_wipe(us_auth, sizeof us_auth); us_an = 0;
    if (r) {
        us_log(r == SUDO_E_NOTADMIN ? "users refused not-admin" : "users refused wrong-password", 0);
        us_say(r == SUDO_E_NOTADMIN ? "Only an admin can do that." : "Wrong password.");
        us_sel = 0; if (settings_live) set_paint();
        return;
    }
    us_leave_secret();
    us_run();
}
static void us_ask(void) {   /* the pending change needs the admin's password: the gate, or the work */
    int g = us_gate();
    if (g == UG_NO) { us_log("users refused not-admin", 0); us_say("Only an admin can do that."); if (settings_live) set_paint(); return; }
    if (g == UG_OK) { us_run(); return; }
    us_leave_secret();
    accounts_wipe(us_auth, sizeof us_auth); us_an = 0;
    us_view = UV_AUTH; us_sel = 0; us_log("users view auth", 0);
    if (settings_live) set_paint();
}
static void us_submit(void) {   /* a filled form: the checks that need no privilege first, then the gate */
    if (us_op == UO_ADD) {
        if (!accounts_name_ok(us_f[0])) { us_say(us_fn[0] ? accounts_reason(ACC_E_NAME) : "Type a name first."); us_log("users refused name", 0); us_sel = 0; set_paint(); return; }
        if (accounts_has(us_f[0])) { us_say("That name is taken."); us_log("users refused taken", 0); us_sel = 0; set_paint(); return; }
    }
    int a = 1, b = 2;   /* the new password and its repeat: fields 1 and 2 in both forms */
    if (!us_eq(us_f[a], us_f[b])) {
        accounts_wipe(us_f[a], sizeof us_f[a]); accounts_wipe(us_f[b], sizeof us_f[b]); us_fn[a] = us_fn[b] = 0;
        us_say("The two passwords differ, nothing changed."); us_log("users refused mismatch", 0); us_sel = a - us_first(); set_paint(); return;
    }
    if (!accounts_pass_ok(us_f[a])) {
        accounts_wipe(us_f[a], sizeof us_f[a]); accounts_wipe(us_f[b], sizeof us_f[b]); us_fn[a] = us_fn[b] = 0;
        us_say(accounts_reason(ACC_E_PASS)); us_log("users refused short", 0); us_sel = a - us_first(); set_paint(); return;
    }
    if (us_op == UO_PASS && us_me(us_who)) {   /* your own: the old password, no admin needed (the Terminal's passwd) */
        us_say("Checking the password..."); set_paint();
        int r = accounts_check(us_who, us_f[0]);
        if (r) {
            login_wait_ms(LOGIN_DELAY_MS);
            accounts_wipe(us_f[0], sizeof us_f[0]); us_fn[0] = 0;
            us_say("Wrong current password."); us_log("users refused wrong-password", 0); us_sel = 0; set_paint(); return;
        }
        us_leave_secret();
        us_run();
        return;
    }
    us_ask();
}
static void us_open_form(int op, const char *who) {
    us_op = op; us_copy(us_who, who, sizeof us_who);
    us_leave_secret();
    us_wipe(); us_view = UV_FORM; us_sel = 0;
    us_log(op == UO_ADD ? "users view add" : "users view passwd", op == UO_ADD ? 0 : who);
    set_paint();
}
static void us_activate(void) {   /* Return, Space, Right or a click on the picked row */
    int ids[4];
    if (us_view == UV_LIST) {
        int c = (int)accounts_count();
        if (us_sel < c) {
            const char *n = accounts_name((unsigned)us_sel);
            if (!us_me(n) && !us_admin_viewer()) { us_say("Only an admin can change other accounts."); us_log("users refused not-admin", 0); set_paint(); return; }
            us_copy(us_who, n, sizeof us_who); us_wipe(); us_view = UV_DETAIL; us_sel = 0; us_log("users view detail", us_who); set_paint();
        } else if (c < (int)ACC_USERS_MAX) {
            if (c && !us_admin_viewer()) { us_say("Only an admin can add users."); us_log("users refused not-admin", 0); set_paint(); return; }
            us_open_form(UO_ADD, "");
        }
    } else if (us_view == UV_DETAIL) {
        us_dids(ids);
        int id = ids[us_sel];
        if (id == UD_PASS) us_open_form(UO_PASS, us_who);
        else if (id == UD_ADMIN) { us_op = UO_ADMIN; us_ask(); }
        else if (id == UD_DELETE) { us_op = UO_DELETE; us_view_set(UV_CONFIRM, 1, "users view confirm"); }
        else us_to_list(us_pos(us_who));
    } else if (us_view == UV_CONFIRM) {
        if (us_sel == 0) us_ask(); else us_view_set(UV_DETAIL, 2, "users view detail");
    } else if (us_view == UV_FORM) {
        if (us_sel == us_nfields()) us_submit();
        else if (us_sel == us_nfields() + 1) { if (us_op == UO_ADD) us_to_list(0); else us_view_set(UV_DETAIL, 0, "users view detail"); }
        else if (us_sel == us_nfields() - 1) us_submit();   /* Return on the last field is the button */
        else { us_sel++; set_paint(); }   /* Return on a field goes on to the next row */
    } else {   /* the admin password */
        if (us_sel == 2) us_to_list(0);
        else us_auth_submit();
    }
}
static void us_back(void) {   /* Esc, Left */
    if (us_view == UV_LIST) { us_focus = 0; us_log("users view sidebar", 0); set_paint(); }
    else if (us_view == UV_DETAIL) us_to_list(us_pos(us_who));
    else if (us_view == UV_CONFIRM) us_view_set(UV_DETAIL, 2, "users view detail");
    else if (us_view == UV_FORM && us_op == UO_PASS) us_view_set(UV_DETAIL, 0, "users view detail");
    else us_to_list(0);
}
static int us_field_at(int row) { return us_view == UV_AUTH ? (row == 0 ? 0 : -1) : (row < us_nfields() ? row + us_first() : -1); }
static void us_type(unsigned code) {
    int row = us_sel, f = us_field_at(row);
    if (f < 0) return;
    char *buf = us_view == UV_AUTH ? us_auth : us_f[f];
    unsigned *len = us_view == UV_AUTH ? &us_an : &us_fn[f];
    int is_name = us_view == UV_FORM && us_op == UO_ADD && f == 0;
    if (code == 14) { if (*len) buf[--*len] = 0; return; }
    int ch = code < 58 ? ask_char(code) : 0;
    if (ch < ' ' || ch > 126) return;
    if (is_name) {
        if (ch >= 'A' && ch <= 'Z') ch += 32;   /* names are lower case */
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return;
    }
    if (*len < (is_name ? ACC_NAME_MAX : ACC_PASS_MAX)) { buf[(*len)++] = (char)ch; buf[*len] = 0; }
}
static int us_key(unsigned code) {   /* a key down with the pane's list or form focused */
    us_msg[0] = 0; us_hush = 0;
    int typing = us_secret_view(), n = us_rows();
    if (code == 1) { us_back(); return 1; }
    if (code == 103) { if (us_sel > 0) us_sel--; set_paint(); return 1; }
    if (code == 108 || code == 15) { us_sel = us_sel + 1 < n ? us_sel + 1 : code == 15 ? 0 : us_sel; set_paint(); return 1; }
    if (code == 28 || code == 96) { us_activate(); return 1; }
    if (typing && us_field_at(us_sel) >= 0 && (code == 14 || (code < 58 && ask_char(code)))) { us_type(code); set_paint(); return 1; }
    if (!typing && (code == 57 || code == 106)) { us_activate(); return 1; }
    if (!typing && code == 105) { us_back(); return 1; }
    return 1;
}
static void us_enter(void) {   /* a key on the sidebar: Return or Right goes into the pane */
    us_focus = 1; us_view = UV_LIST; us_sel = 0; us_log("users view list", 0); set_paint();
}
static void us_click(int lx, int ly, int cx, int cy) {   /* a click on the card: pick the row and do it, but a field only takes the keys */
    int n = us_rows();
    if (lx < cx || lx >= cx + SET_CARD_W || ly < cy || ly >= cy + n * SET_ROW_H) return;
    us_focus = 1; us_msg[0] = 0; us_hush = 0;
    us_sel = (ly - cy) / SET_ROW_H;
    if (us_secret_view() && us_field_at(us_sel) >= 0) { set_paint(); return; }
    us_activate();
}

/* ---- drawing: the same rows, radii and colours as the other sections ---- */
static const char *us_title(void) {
    if (us_view == UV_DETAIL) return us_who;
    if (us_view == UV_FORM) return us_op == UO_ADD ? "Add user" : "Change password";
    if (us_view == UV_AUTH) return "Admin password";
    if (us_view == UV_CONFIRM) return "Remove this user?";
    return set_names[SET_USERS];
}
static void us_stars(char *b, unsigned n) { unsigned k = 0; for (; k < n && k < 24; k++) b[k] = '*'; b[k] = 0; }
static void us_paint(int cx, int cy, int wy, int wh) {
    int n = us_rows(), ids[4], hy = cy + n * SET_ROW_H + 8;
    us_dids(ids);
    gui_rounded_rect_on_wallpaper(cx, cy, SET_CARD_W, n * SET_ROW_H, 0x00FFFFFF, 10);
    for (int r = 0; r < n; r++) {
        int ry = cy + r * SET_ROW_H, on = us_focus && r == us_sel, toggle = -1;
        char label[ACC_NAME_MAX + 24], v[ACC_PASS_MAX + 2]; label[0] = v[0] = 0;
        if (us_view == UV_LIST) {
            if (r < (int)accounts_count()) {
                const char *nm = accounts_name((unsigned)r); int me = us_me(nm), ad = sudo_is_admin(nm);
                US_CAT(label, nm);
                US_CAT(v, me && ad ? "You, admin" : me ? "You" : ad ? "Admin" : "");
            } else US_CAT(label, "Add user");
        } else if (us_view == UV_DETAIL) {
            int id = ids[r];
            US_CAT(label, id == UD_PASS ? "Change password" : id == UD_ADMIN ? "Admin" : id == UD_DELETE ? "Remove user" : "Back");
            if (id == UD_ADMIN) toggle = sudo_is_admin(us_who);
        } else if (us_view == UV_CONFIRM) {
            US_CAT(label, r == 0 ? "Remove " : "Cancel"); if (r == 0) US_CAT(label, us_who);
        } else if (us_view == UV_AUTH) {
            if (r == 0) { US_CAT(label, "Password for "); US_CAT(label, current_user); us_stars(v, us_an); }
            else US_CAT(label, r == 1 ? "Continue" : "Cancel");
        } else {
            int f = us_field_at(r);
            if (f >= 0) {
                static const char *const add[3] = { "Name", "Password", "Again" }, *const pw[3] = { "Current password", "New password", "Again" };
                US_CAT(label, us_op == UO_ADD ? add[f] : pw[f]);
                if (us_op == UO_ADD && f == 0) us_copy(v, us_f[0], sizeof v);
                else us_stars(v, us_fn[f]);
            } else US_CAT(label, r == us_nfields() ? (us_op == UO_ADD ? "Add user" : "Change password") : "Cancel");
        }
        if (r) gui_hairline_h(cx + 12, ry, SET_CARD_W - 24, 0x00E8E2D8);
        gui_text(label, cx + 14, ry + 7, on ? SET_ACCENT : SET_INK);
        if (toggle >= 0) {   /* the Mute row's switch */
            int tx = cx + SET_CARD_W - 14 - 26;
            gui_rounded_rect_on_wallpaper(tx, ry + 7, 26, 15, toggle ? SET_ACCENT : 0x00C7C0B4, 7);
            gui_rounded_rect_on_wallpaper(toggle ? tx + 13 : tx + 2, ry + 9, 11, 11, 0x00FFFFFF, 5);
        } else if (us_secret_view() && us_field_at(r) >= 0) {
            int w = gui_text_width(v), x = cx + SET_CARD_W - 14 - w - 2;
            gui_text(v, x, ry + 7, SET_INK);
            if (on) window_rect(cx + SET_CARD_W - 14 - 1, ry + 6, 1, 16, SET_ACCENT);   /* the caret */
        } else if (v[0]) gui_text(v, cx + SET_CARD_W - 14 - gui_text_width(v), ry + 7, SET_HINT);
    }
    const char *hint = "";
    if (us_view == UV_LIST) hint = accounts_count() ? (us_focus ? "Return opens, Esc goes back." : "Return opens the list.") : "No accounts yet. The first one is an admin.";
    else if (us_view == UV_DETAIL) hint = us_admin_viewer() ? "An admin's password is asked for each change." : "You can change your own password.";
    else if (us_view == UV_FORM) hint = us_op == UO_ADD ? "Four to 64 characters. Return goes on." : "Your own needs the old one.";
    else if (us_view == UV_AUTH) hint = "Asked like sudo, then kept for five minutes.";
    else hint = "The files in their home folder stay on the card.";
    if (n <= 6) gui_text(hint, cx, hy, SET_HINT);
    char m[sizeof us_msg]; us_copy(m, us_msg, sizeof m);
    while (m[0] && gui_text_width(m) > SET_CARD_W + 6) { unsigned k = 0; while (m[k]) k++; m[k - 1] = 0; }
    gui_text(m, cx, wy + wh - 32, SET_ACCENT);
}
